import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "abhpc-lbs"
FAKE_COMMAND = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

directory = Path(os.environ["LBS_FIXTURE"])
data = json.loads((directory / "data.json").read_text())
command = Path(sys.argv[0]).name
args = sys.argv[1:]
with (directory / "calls.jsonl").open("a") as log:
    log.write(json.dumps({"command": command, "args": args,
                          "executable": sys.argv[0]}) + "\n")
assert command == "scontrol", command
if args == ["--local", "--oneliner", "show", "nodes"]:
    print(data["records"], end="")
    sys.exit(data.get("query_status", 0))
assert args[:2] == ["--local", "update"], args
assert len(args) == 4, args
sys.exit(data.get("update_status", 0))
'''


def record(name, cpus=8, weight=20, load="3.00", state="IDLE"):
    return (f"NodeName={name} Arch=x86_64 CPUTot={cpus} CPUAlloc=0 "
            f"CPULoad={load} State={state} Weight={weight} Partitions=batch,debug\n")


class CLITest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        for name in ("scontrol", "pdsh", "ssh", "sinfo"):
            executable = self.bin / name
            executable.write_text(FAKE_COMMAND)
            executable.chmod(0o755)
        self.config = self.directory / "lbs.conf"
        self.config.write_text("")
        self.data = {"records": record("node1") + record("node10")}
        self.environment = dict(os.environ, LBS_FIXTURE=str(self.directory),
                                PATH=str(self.bin) + os.pathsep + os.environ["PATH"])

    def run_lbs(self, *args, ok=True, env=None):
        (self.directory / "data.json").write_text(json.dumps(self.data))
        result = subprocess.run([str(BINARY), "-c", str(self.config), *args],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                universal_newlines=True, timeout=30,
                                env=env or self.environment)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stderr)
        return result

    def calls(self, command=None):
        log = self.directory / "calls.jsonl"
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return [call for call in calls if command is None or call["command"] == command]

    def updates(self):
        return [call for call in self.calls("scontrol") if "update" in call["args"]]

    def weights(self, result):
        return {line.split()[0]: int(line.split()[-1])
                for line in result.stdout.splitlines()[1:]}

    def test_default_formula_exact_host_matching_and_truncation(self):
        self.data["records"] = record("node1", load="4") + record("node10", load="2")
        result = self.run_lbs("--dry-run")
        self.assertEqual(self.weights(result), {"node1": 42, "node10": 23})
        self.assertEqual(self.updates(), [])

    def test_expression_operators_variables_and_precedence(self):
        formulas = {
            "load + 2 * load": 9,
            "(load + 2) * load": 15,
            "100 * load / cpus + weight": 57,
            "-(weight - 2 * +load) + 30": 16,
            "1e1 + .5 * weight": 20,
            "(weight - 20) / cpus": 0,
        }
        for formula, expected in formulas.items():
            with self.subTest(formula=formula):
                result = self.run_lbs("-n", "-f", formula)
                self.assertEqual(self.weights(result)["node1"], expected)

    def test_config_reload_cli_precedence_and_limits(self):
        self.config.write_text("# custom policy\nformula = 500\nmin_weight=10\n"
                               "max_weight=80\n")
        self.assertEqual(self.weights(self.run_lbs("-n"))["node1"], 80)
        self.config.write_text(self.config.read_text().replace("500", "-500"))
        self.assertEqual(self.weights(self.run_lbs("-n"))["node1"], 10)
        self.assertEqual(self.weights(self.run_lbs("-n", "-f", "42"))["node1"], 42)

    def test_custom_scontrol_path_used_for_queries_and_updates(self):
        slurm_bin = self.directory / "slurm install" / "bin"
        slurm_bin.mkdir(parents=True)
        executable = slurm_bin / "scontrol"
        (self.bin / "scontrol").rename(executable)
        self.config.write_text("scontrol_path = " + str(executable) + "\n")
        self.run_lbs("-f", "42")
        calls = self.calls("scontrol")
        self.assertEqual(len(calls), 2)
        self.assertEqual([call["executable"] for call in calls], [str(executable)] * 2)
        self.assertEqual(self.updates()[0]["args"][-1], "Weight=42")

    def test_unusable_scontrol_path_never_falls_back_to_path(self):
        executable = self.directory / "scontrol"
        for present in (False, True):
            with self.subTest(present=present):
                if present:
                    executable.write_text("#!/bin/sh\nexit 0\n")
                    executable.chmod(0o644)
                self.config.write_text("scontrol_path = " + str(executable) + "\n")
                result = self.run_lbs(ok=False)
                self.assertIn(str(executable), result.stderr)
        self.assertEqual(self.calls(), [])

    def test_large_cluster_uses_one_query_and_bounded_batches(self):
        names = [f"node{i:05}" for i in range(10000)]
        self.data["records"] = "".join(record(name) for name in reversed(names))
        self.run_lbs("-f", "42")
        queries = [call for call in self.calls("scontrol") if "show" in call["args"]]
        self.assertEqual(len(queries), 1)
        updates = self.updates()
        self.assertLess(len(updates), 10)
        updated = []
        for call in updates:
            self.assertEqual(call["args"][-1], "Weight=42")
            self.assertLess(len(call["args"][-2]), 32768)
            updated.extend(call["args"][-2].partition("=")[2].split(","))
        self.assertEqual(updated, names)

    def test_groups_by_weight_and_omits_unchanged(self):
        self.data["records"] = (record("node1", load="1") + record("node10", load="2") +
                                record("node2", load="1") + record("same", weight=42, load="1"))
        self.run_lbs("-f", "load + 41")
        updates = self.updates()
        self.assertEqual(len(updates), 2)
        self.assertEqual(updates[0]["args"][-2:], ["NodeName=node1,node2", "Weight=42"])
        self.assertEqual(updates[1]["args"][-2:], ["NodeName=node10", "Weight=43"])

    def test_missing_or_invalid_controller_load_keeps_old_weight(self):
        for load in ("N/A", "nan", "inf", "-1", "3.0junk", "1e9999", ""):
            with self.subTest(load=load):
                self.data["records"] = record("node1", load=load) + record("node10")
                result = self.run_lbs("-f", "42", ok=False)
                self.assertIn("skipped=1", result.stderr)
                self.assertEqual(self.updates()[-1]["args"][-2:],
                                 ["NodeName=node10", "Weight=42"])
        self.data["records"] = record("node1").replace("CPULoad=3.00 ", "") + record("node10")
        result = self.run_lbs("-n", ok=False)
        self.assertNotIn("node1", self.weights(result))

    def test_zero_load_is_valid(self):
        self.data["records"] = record("node1", load="0")
        self.assertEqual(self.weights(self.run_lbs("-n")), {"node1": 5})

    def test_unavailable_nodes_do_not_use_cached_load(self):
        for state in ("DOWN", "UNKNOWN", "FUTURE", "ERROR", "IDLE+NOT_RESPONDING", "IDLE*",
                      "IDLE+POWERED_DOWN", "IDLE+POWERING_UP", "IDLE+POWERING_DOWN",
                      "ALLOCATED+FAIL", "IDLE+INVALID_REG", "IDLE+REBOOT_ISSUED"):
            with self.subTest(state=state):
                self.data["records"] = record("node1", state=state) + record("node10")
                result = self.run_lbs("-f", "42")
                self.assertIn("skipped=1", result.stderr)
                self.assertEqual(self.updates()[-1]["args"][-2:],
                                 ["NodeName=node10", "Weight=42"])

    def test_all_unavailable_nodes_are_successful_noop(self):
        self.data["records"] = (record("node1", state="DOWN+NOT_RESPONDING") +
                                record("node10", state="DOWN", load="N/A"))
        result = self.run_lbs("-f", "42")
        self.assertIn("changed=0 skipped=2", result.stderr)
        self.assertEqual(self.updates(), [])

    def test_missing_node_state_is_still_an_error(self):
        for records in (record("node1", state=""),
                        record("node1").replace("State=IDLE ", "")):
            with self.subTest(records=records):
                self.data["records"] = records
                self.run_lbs("-n", ok=False)
        self.assertEqual(self.updates(), [])

    def test_live_node_states_remain_eligible(self):
        states = ("IDLE", "MIXED", "ALLOCATED", "IDLE+DRAIN", "ALLOCATED+COMPLETING")
        self.data["records"] = "".join(record("node" + str(i), state=state)
                                       for i, state in enumerate(states))
        self.assertEqual(len(self.weights(self.run_lbs("-n"))), len(states))

    def test_only_scontrol_is_used(self):
        self.run_lbs("-f", "42")
        self.assertEqual([call["command"] for call in self.calls()], ["scontrol", "scontrol"])

    def test_division_by_zero_and_overflow_skip_nodes(self):
        for formula in ("1 / (weight - 20)", "1e308 * 1e308"):
            with self.subTest(formula=formula):
                result = self.run_lbs("-f", formula, ok=False)
                self.assertIn("skipped=2", result.stderr)
        self.assertEqual(self.updates(), [])

    def test_bad_formulas_fail_before_external_commands(self):
        for formula in ("", "unknown + 1", "load; touch /tmp/injection", "1 +",
                        "(1+2", "1 2", "1e999", "2**3", "-" * 100 + "1", "1" * 1025,
                        "load1", "load5", "load15"):
            with self.subTest(formula=formula):
                self.run_lbs("-f", formula, ok=False)
        self.assertEqual(self.calls(), [])

    def test_bad_config_fails_before_external_commands(self):
        for config in ("formula", "unexpected=1", "fanout=32", "connect_timeout=5", "command_timeout=10",
                       "max_weight=4294967295", "min_weight=11\nmax_weight=10",
                       "max_weight=1.5",
                       "scontrol_path=", "scontrol_path=./scontrol", "scontrol_path=slurm/bin",
                       "scontrol_path=/" + "a" * 4096):
            with self.subTest(config=config):
                self.config.write_text(config)
                self.run_lbs(ok=False)
        self.assertEqual(self.calls(), [])

    def test_bad_node_records_make_no_updates(self):
        for records in (record("bad;name"), record("ALL"), record("node", cpus=0),
                        record("node", weight=-1), "NodeName=node CPUTot=8\n",
                        record("node") * 2, ""):
            with self.subTest(records=records):
                self.data["records"] = records
                self.run_lbs(ok=False)
        self.assertEqual(self.updates(), [])

    def test_query_failure_makes_no_updates(self):
        self.data["query_status"] = 1
        self.run_lbs(ok=False)
        self.assertEqual(self.updates(), [])

    def test_failed_update_stops_later_batches(self):
        self.data["update_status"] = 1
        self.data["records"] = record("node1", load="1") + record("node10", load="2")
        result = self.run_lbs("-f", "load + 41", ok=False)
        self.assertIn("earlier batches may have been applied", result.stderr)
        self.assertEqual(len(self.updates()), 1)

    def test_missing_config_is_reported(self):
        self.config.unlink()
        self.run_lbs(ok=False)
        self.assertEqual(self.calls(), [])

if __name__ == "__main__":
    unittest.main()
