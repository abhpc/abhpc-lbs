#include "formula.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    const char *p;
    const double *values;
    const char *error;
    unsigned depth;
};

static void spaces(struct parser *p)
{
    while (isspace((unsigned char)*p->p))
        p->p++;
}

static double expression(struct parser *p);

static double atom(struct parser *p)
{
    double value = 0;
    spaces(p);
    if (++p->depth > 64) {
        p->error = "expression nesting exceeds 64";
        p->depth--;
        return 0;
    }
    if (*p->p == '+' || *p->p == '-') {
        char sign = *p->p++;
        value = atom(p);
        if (sign == '-')
            value = -value;
    } else if (*p->p == '(') {
        p->p++;
        value = expression(p);
        spaces(p);
        if (*p->p == ')')
            p->p++;
        else
            p->error = "missing closing parenthesis";
    } else if (isalpha((unsigned char)*p->p)) {
        static const char *const names[] = {"load", "cpus", "weight"};
        const char *start = p->p;
        while (isalnum((unsigned char)*p->p) || *p->p == '_')
            p->p++;
        size_t i;
        for (i = 0; i < 3; i++) {
            if (strlen(names[i]) == (size_t)(p->p - start) &&
                strncmp(start, names[i], (size_t)(p->p - start)) == 0) {
                value = p->values ? p->values[i] : 0;
                break;
            }
        }
        if (i == 3)
            p->error = "unknown variable (supported: load, cpus, weight)";
    } else if (isdigit((unsigned char)*p->p) || *p->p == '.') {
        char *end;
        errno = 0;
        value = strtod(p->p, &end);
        if (end == p->p || errno || !isfinite(value))
            p->error = "invalid numeric literal";
        p->p = end;
    } else {
        p->error = "expected a number, variable or parenthesis";
    }
    p->depth--;
    return value;
}

static double product(struct parser *p)
{
    double value = atom(p);
    spaces(p);
    while (!p->error && (*p->p == '*' || *p->p == '/')) {
        char op = *p->p++;
        double rhs = atom(p);
        if (p->values && op == '/' && rhs == 0) {
            p->error = "division by zero";
            return 0;
        }
        if (p->values)
            value = op == '*' ? value * rhs : value / rhs;
        if (p->values && !isfinite(value))
            p->error = "non-finite result";
        spaces(p);
    }
    return value;
}

static double expression(struct parser *p)
{
    double value = product(p);
    spaces(p);
    while (!p->error && (*p->p == '+' || *p->p == '-')) {
        char op = *p->p++;
        double rhs = product(p);
        value = op == '+' ? value + rhs : value - rhs;
        if (p->values && !isfinite(value))
            p->error = "non-finite result";
        spaces(p);
    }
    return value;
}

const char *formula_eval(const char *formula, const double values[3], double *result)
{
    if (strlen(formula) > FORMULA_MAX)
        return "formula exceeds 1024 characters";
    struct parser p = {.p = formula, .values = values};
    *result = expression(&p);
    spaces(&p);
    if (!p.error && *p.p)
        p.error = "unexpected trailing input";
    return p.error;
}
