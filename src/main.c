#define _POSIX_C_SOURCE 200809L
#include "formula.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "1.0.0"
#endif
#ifndef CONFIG_PATH
#define CONFIG_PATH "/etc/abhpc-lbs.conf"
#endif

#define WEIGHT_MAX (UINT32_MAX - 2)
#define BATCH_SIZE 32768

struct config {
    char formula[FORMULA_MAX + 1];
    char scontrol_path[PATH_MAX];
    unsigned min_weight, max_weight;
};

struct node {
    char *name;
    unsigned cpus, weight, next_weight;
    double load;
    bool unavailable, load_valid, update;
};

static void die(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "abhpc-lbs: ");
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

static void *allocate(void *old, size_t size)
{
    void *p = realloc(old, size);
    if (!p)
        die("out of memory");
    return p;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static bool number(const char *text, unsigned max, unsigned *result)
{
    if (!isdigit((unsigned char)*text))
        return false;
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n > max)
        return false;
    *result = (unsigned)n;
    return true;
}

static void set_option(struct config *config, const char *key, const char *value)
{
    if (strcmp(key, "scontrol_path") == 0) {
        if (!*value || strlen(value) >= sizeof(config->scontrol_path) ||
            (strcmp(value, "scontrol") != 0 && *value != '/'))
            die("scontrol_path must be 'scontrol' or an absolute executable path");
        strcpy(config->scontrol_path, value);
        return;
    }
    if (strcmp(key, "formula") == 0) {
        if (strlen(value) > FORMULA_MAX)
            die("formula exceeds %d characters", FORMULA_MAX);
        strcpy(config->formula, value);
        return;
    }
    unsigned *target;
    if (strcmp(key, "min_weight") == 0) {
        target = &config->min_weight;
    } else if (strcmp(key, "max_weight") == 0) {
        target = &config->max_weight;
    } else {
        die("unknown configuration key: %s", key);
    }
    if (!number(value, WEIGHT_MAX, target))
        die("invalid value for %s: %s", key, value);
}

static void read_config(struct config *config, const char *path, bool required)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT && !required)
            return;
        die("cannot open %s: %s", path, strerror(errno));
    }
    char *line = NULL;
    size_t size = 0, lineno = 0;
    while (getline(&line, &size, file) != -1) {
        lineno++;
        char *comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        char *key = trim(line);
        if (!*key)
            continue;
        char *value = strchr(key, '=');
        if (!value)
            die("%s:%zu: expected key = value", path, lineno);
        *value++ = '\0';
        set_option(config, trim(key), trim(value));
    }
    if (ferror(file))
        die("cannot read %s", path);
    free(line);
    fclose(file);
}

static FILE *temporary(void)
{
    FILE *file = tmpfile();
    if (!file || fcntl(fileno(file), F_SETFD, FD_CLOEXEC) == -1)
        die("cannot create temporary file: %s", strerror(errno));
    return file;
}

static void reset_file(FILE *file)
{
    if (fflush(file) || fseek(file, 0, SEEK_SET))
        die("temporary file I/O failed: %s", strerror(errno));
}

/* File-backed output avoids pipe deadlocks with large node lists. */
static int run(char *const argv[], FILE *output)
{
    pid_t pid = fork();
    if (pid == -1)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        int in = open("/dev/null", O_RDONLY);
        if (in < 0 || dup2(in, STDIN_FILENO) < 0 ||
            (output && dup2(fileno(output), STDOUT_FILENO) < 0)) {
            perror("abhpc-lbs: redirect");
            _exit(126);
        }
        if (in > STDERR_FILENO)
            close(in);
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR)
            die("waitpid: %s", strerror(errno));
    }
    if (output)
        reset_file(output);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "abhpc-lbs: %s failed (status %d)\n", argv[0],
                WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        return 1;
    }
    return 0;
}

static bool valid_name(const char *name)
{
    if (!isalnum((unsigned char)*name) || strlen(name) > 255 ||
        strcasecmp(name, "ALL") == 0)
        return false;
    for (; *name; name++) {
        if (!isalnum((unsigned char)*name) && !strchr("-_.", *name))
            return false;
    }
    return true;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct node *)a)->name, ((const struct node *)b)->name);
}

static bool unavailable_state(const char *state)
{
    if (!state)
        return false;
    static const char *const unavailable[] = {
        "DOWN", "UNKNOWN", "FUTURE", "ERROR", "NOT_RESPONDING", "NO_RESPOND",
        "POWER", "FAIL", "INVALID_REG", "REBOOT_ISSUED"
    };
    for (size_t i = 0; i < sizeof(unavailable) / sizeof(*unavailable); i++) {
        if (strstr(state, unavailable[i]))
            return true;
    }
    return strpbrk(state, "*~#!%?") != NULL;
}

static bool usable_load(const char *load, double *value)
{
    if (!load)
        return false;
    char *end;
    errno = 0;
    *value = strtod(load, &end);
    return end != load && !*end && !errno && isfinite(*value) && *value >= 0;
}

static struct node *read_nodes(size_t *count, char *scontrol_path)
{
    char *argv[] = {scontrol_path, "--local", "--oneliner", "show", "nodes", NULL};
    FILE *file = temporary();
    if (run(argv, file))
        die("node query failed; no weights changed");
    struct node *nodes = NULL;
    size_t capacity = 0, size = 0;
    char *line = NULL;
    while (getline(&line, &size, file) != -1) {
        char *name = NULL, *cpus = NULL, *weight = NULL, *load = NULL, *state = NULL, *save;
        if (!*trim(line))
            continue;
        for (char *word = strtok_r(line, " \t\r\n", &save); word;
             word = strtok_r(NULL, " \t\r\n", &save)) {
            if (strncmp(word, "NodeName=", 9) == 0)
                name = word + 9;
            else if (strncmp(word, "CPUTot=", 7) == 0)
                cpus = word + 7;
            else if (strncmp(word, "Weight=", 7) == 0)
                weight = word + 7;
            else if (strncmp(word, "CPULoad=", 8) == 0)
                load = word + 8;
            else if (strncmp(word, "State=", 6) == 0)
                state = word + 6;
        }
        struct node node = {0};
        if (!name || !cpus || !weight || !valid_name(name) ||
            !number(cpus, WEIGHT_MAX, &node.cpus) || !node.cpus ||
            !number(weight, WEIGHT_MAX, &node.weight))
            die("invalid node record; no weights changed");
        node.unavailable = unavailable_state(state);
        node.load_valid = state && *state && usable_load(load, &node.load);
        node.name = strdup(name);
        if (!node.name)
            die("out of memory");
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 128;
            nodes = allocate(nodes, capacity * sizeof(*nodes));
        }
        nodes[(*count)++] = node;
    }
    if (ferror(file))
        die("cannot read node query output");
    free(line);
    fclose(file);
    if (!*count)
        die("no nodes returned; no weights changed");
    qsort(nodes, *count, sizeof(*nodes), by_name);
    for (size_t i = 1; i < *count; i++) {
        if (strcmp(nodes[i - 1].name, nodes[i].name) == 0)
            die("duplicate node: %s; no weights changed", nodes[i].name);
    }
    return nodes;
}

static int by_weight(const void *a, const void *b)
{
    const struct node *na = a, *nb = b;
    if (na->update != nb->update)
        return na->update ? -1 : 1;
    if (na->next_weight != nb->next_weight)
        return na->next_weight < nb->next_weight ? -1 : 1;
    return by_name(a, b);
}

static int update_nodes(struct node *nodes, size_t count, char *scontrol_path)
{
    qsort(nodes, count, sizeof(*nodes), by_weight);
    for (size_t i = 0; i < count && nodes[i].update;) {
        unsigned weight = nodes[i].next_weight;
        char names[BATCH_SIZE] = "NodeName=", value[32];
        size_t used = strlen(names), batch = 0;
        while (i < count && nodes[i].update && nodes[i].next_weight == weight) {
            size_t len = strlen(nodes[i].name);
            if (used + len + (batch ? 1 : 0) >= sizeof(names))
                break;
            if (batch)
                names[used++] = ',';
            memcpy(names + used, nodes[i].name, len + 1);
            used += len;
            batch++;
            i++;
        }
        snprintf(value, sizeof(value), "Weight=%u", weight);
        char *argv[] = {scontrol_path, "--local", "update", names, value, NULL};
        if (run(argv, NULL)) {
            fprintf(stderr, "abhpc-lbs: update failed for %zu nodes at weight %u; "
                    "stopping, earlier batches may have been applied\n", batch, weight);
            return 1;
        }
        fprintf(stderr, "abhpc-lbs: updated %zu nodes to weight %u\n", batch, weight);
    }
    return 0;
}

static void usage(void)
{
    puts("Usage: abhpc-lbs [options]\n"
         "  -c, --config PATH     Configuration file (default: " CONFIG_PATH ")\n"
         "  -f, --formula EXPR    Override the weight formula\n"
         "  -n, --dry-run         Print calculations without updating Slurm\n"
         "  -h, --help            Show help\n"
         "  -V, --version         Show version\n"
         "Variables: load (Slurm CPULoad), cpus, weight; operators: + - * / ( )");
}

int main(int argc, char **argv)
{
    struct config config = {
        .formula = "0.75 * 100 * load / cpus + 0.25 * weight",
        .scontrol_path = "scontrol",
        .min_weight = 0, .max_weight = 100
    };
    const char *path = CONFIG_PATH, *formula = NULL;
    bool explicit_config = false, dry_run = false;
    const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"formula", required_argument, NULL, 'f'},
        {"dry-run", no_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "c:f:nhV", options, NULL)) != -1) {
        switch (option) {
        case 'c': path = optarg; explicit_config = true; break;
        case 'f': formula = optarg; break;
        case 'n': dry_run = true; break;
        case 'h': usage(); return 0;
        case 'V': puts("abhpc-lbs " VERSION); return 0;
        default: return EXIT_FAILURE;
        }
    }
    if (optind != argc)
        die("unexpected argument: %s", argv[optind]);
    read_config(&config, path, explicit_config);
    if (formula)
        set_option(&config, "formula", formula);
    if (config.min_weight > config.max_weight)
        die("min_weight must not exceed max_weight");
    double result;
    const char *error = formula_eval(config.formula, NULL, &result);
    if (error)
        die("invalid formula: %s", error);
    size_t count = 0, changed = 0, skipped = 0;
    struct node *nodes = read_nodes(&count, config.scontrol_path);
    int failed = 0;
    if (dry_run)
        puts("NODE CPUS LOAD OLD_WEIGHT NEW_WEIGHT");
    for (size_t i = 0; i < count; i++) {
        struct node *node = &nodes[i];
        if (node->unavailable || !node->load_valid) {
            fprintf(stderr, "abhpc-lbs: %s: %s; keeping weight %u\n", node->name,
                    node->unavailable ? "node unavailable" : "missing/invalid CPULoad or State",
                    node->weight);
            skipped++;
            if (!node->unavailable)
                failed = 1;
            continue;
        }
        double values[] = {node->load, node->cpus, node->weight};
        error = formula_eval(config.formula, values, &result);
        if (error) {
            fprintf(stderr, "abhpc-lbs: %s: %s; keeping weight %u\n",
                    node->name, error, node->weight);
            skipped++;
            failed = 1;
            continue;
        }
        if (result < config.min_weight)
            result = config.min_weight;
        if (result > config.max_weight)
            result = config.max_weight;
        node->next_weight = (unsigned)result;
        node->update = node->next_weight != node->weight;
        changed += node->update;
        if (dry_run)
            printf("%s %u %.2f %u %u\n", node->name, node->cpus, node->load,
                   node->weight, node->next_weight);
    }
    if (!dry_run)
        failed |= update_nodes(nodes, count, config.scontrol_path);
    fprintf(stderr, "abhpc-lbs: nodes=%zu changed=%zu skipped=%zu dry_run=%s\n",
            count, changed, skipped, dry_run ? "yes" : "no");
    for (size_t i = 0; i < count; i++)
        free(nodes[i].name);
    free(nodes);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
