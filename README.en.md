# abhpc-lbs

[中文](README.md) | English

A C program that adjusts Slurm node weights based on historical load. Supports EL7 and EL8.
It reads load, CPU count and weight for all nodes in one `scontrol` query, then batches
updates by weight. Nodes with invalid load or calculation errors keep their current weight.

## Build and run

Building requires GCC and Make. Tests require Python 3.6+. At runtime, the program
uses Slurm's `scontrol`.

Run as a user with permission to update Slurm nodes. For a custom Slurm installation,
set `scontrol_path` in the configuration. Set `SLURM_CONF` and the authentication
environment as required by your cluster.

```bash
make
make test

# Preview calculated weights
./abhpc-lbs --config ./abhpc-lbs.conf --dry-run

# Update weights once
./abhpc-lbs --config ./abhpc-lbs.conf
```

## Configuration

The default file is `/etc/abhpc-lbs.conf`. The program reads it on each run, so edits
take effect on the next run. If the default file is absent, built-in defaults apply.
A file specified with `--config` must exist.

```ini
scontrol_path = scontrol
state_file = /var/lib/abhpc-lbs/history
history_fast_minutes = 30
history_slow_minutes = 80
formula = history
min_weight = 0
max_weight = 100
```

The default `scontrol_path = scontrol` searches `PATH`. For a custom installation,
use the executable's absolute path without quotes:

```ini
scontrol_path = /opt/slurm/bin/scontrol
```

Both queries and updates use this path, including runs started by the timer.
A missing or non-executable file causes an error.

| Variable | Meaning |
| --- | --- |
| `load` | The controller's recorded `CPULoad`, from the same source as `sinfo %O` |
| `cpus` | Slurm `CPUTot`, which may include hardware threads |
| `weight` | The Slurm node weight read at the start of this run |
| `history` | Smoothed historical load score normalized by CPU count; 100 means load equals CPU count |

Formulas support numbers, `+ - * /`, parentheses and unary signs. Results are clamped
to the configured weight limits, then truncated to integers. Configuration files
support `#` comments; do not quote formulas. Command-line options override the file:

```bash
./abhpc-lbs -c ./abhpc-lbs.conf \
  --formula '0.9 * history + 0.1 * 100 * load / cpus' \
  --dry-run
```

`load` is the five-minute load average last reported by Slurm. It may lag and is
not CPU utilization. The program applies two stages of exponential smoothing using
elapsed time, with default time constants of 30 and 80 minutes. Both settings are
in minutes and must satisfy `0 < history_fast_minutes < history_slow_minutes`.
The timer interval is independent; small variations in sampling intervals do not
change the time constants.

With regular sampling, the default cumulative influence, including Slurm's own
smoothing, is approximately:

| Time before the current run | Cumulative influence |
| --- | ---: |
| Last 1 hour | 29% |
| Last 2 hours | 63% |
| Last 6 hours | 98% |

These estimates assume weights have not reached their configured limits.
`state_file` stores floating-point history across restarts. Values are truncated
only when writing Slurm weights. The first run initializes history from existing
Slurm weights; allow about six hours of continuous sampling to build the history.
Load during gaps cannot be reconstructed, so keep sampling regularly.
`--dry-run` reads history without writing the state file. For a custom state path,
create a writable parent directory first.

## RPM

Install the build dependencies on the target system, then run `make rpm`:

```bash
# EL7
sudo yum install gcc make python3 rpm-build
make rpm
```

```bash
# EL8
sudo dnf install gcc make python3 rpm-build
make rpm
```

You can also build each version with Docker. Both targets run the tests:

```bash
make rpm-el7
make rpm-el8
```

| Build | Binary package directory | Source package directory |
| --- | --- | --- |
| Native | `build/rpm/RPMS/` | `build/rpm/SRPMS/` |
| EL7 container | `build/rpm-el7/RPMS/` | `build/rpm-el7/SRPMS/` |
| EL8 container | `build/rpm-el8/RPMS/` | `build/rpm-el8/SRPMS/` |

Build separately for EL7 and EL8, and install the matching package to avoid
dependencies on a newer glibc. Have the Slurm client installed before proceeding:

```bash
# EL7
sudo yum install ./build/rpm-el7/RPMS/*/abhpc-lbs-1.1.0-1.el7.*.rpm
# EL8
sudo dnf install ./build/rpm-el8/RPMS/*/abhpc-lbs-1.1.0-1.el8.*.rpm
```

The EL7 container uses [CentOS Vault](https://vault.centos.org/7.9.2009/) and the
[EPEL 7 archive](https://archives.fedoraproject.org/pub/archive/epel/7/x86_64/).
RPM does not require a fixed Slurm installation path; point the configuration at
an existing manual installation. Python is needed only for tests, not at runtime.

## Scheduled runs

Use `sudo make install` for a source installation. Both source and RPM installations
include a timer and preserve existing configuration. Enable the timer manually:

```bash
sudo systemctl daemon-reload
sudo systemctl enable abhpc-lbs.timer
sudo systemctl start abhpc-lbs.timer
systemctl list-timers abhpc-lbs.timer
journalctl -u abhpc-lbs.service
```

The service runs as root by default. The timer first runs one minute after boot,
then one minute after each run finishes. To change the interval, use
`systemctl edit abhpc-lbs.timer`. For example, to run every five minutes after completion:

```ini
[Timer]
OnUnitInactiveSec=
OnUnitInactiveSec=5min
```

Then run `systemctl daemon-reload` and `systemctl restart abhpc-lbs.timer`.
Set the service user or environment through `systemctl edit abhpc-lbs.service`.
The service user must be able to write to the state file's directory.
You can use cron instead; enable only one scheduler to avoid duplicate runs.

## Operational notes

- Weights affect future scheduling and do not migrate running jobs. Other conditions
  being equal, Slurm prefers lower-weight nodes. See the
  [Slurm Weight definition](https://slurm.schedmd.com/slurm.conf.html#OPT_Weight).
- Nodes that are offline, unresponsive or changing power state are skipped with
  their weights preserved; these skips do not fail the run. Invalid load or state
  fields on an online node, or formula errors, preserve that node's weight and cause
  a nonzero exit status. A failed update batch stops subsequent updates; completed
  updates are not rolled back.
- Corrupt or unwritable history, or a history lock held by another run, causes
  the program to exit with an error.
- A Slurm restart or reconfiguration may restore node weights. The next run uses
  the values it reads. Check results with `--dry-run` before deployment.

This project uses the [MIT license](LICENSE).
