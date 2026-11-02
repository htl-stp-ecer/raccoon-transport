"""Contract check against the shared vectors in contract/check_vectors.json."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from raccoon_transport import contract_check as cc  # noqa: E402

VECTORS = json.loads((ROOT / "contract" / "check_vectors.json").read_text())
# Version entries for every component name the tests below use (their version is "v1").
VERSIONS = json.dumps({n: "v1" for n in ("raccoon-lib", "raccoon-cli", "stm32-data-reader", "a", "b")})


class VectorTests(unittest.TestCase):
    def test_vectors(self):
        defaults = VECTORS["defaults"]
        for case in VECTORS["cases"]:
            with self.subTest(case["name"]), tempfile.TemporaryDirectory() as tmp:
                tmp = Path(tmp)
                marker = tmp / "image"
                if case["on_device"]:
                    marker.mkdir()
                bundle = tmp / "etc" / "raccoon-bundle.json"
                if case["bundle"] is not None:
                    bundle.parent.mkdir()
                    bundle.write_text(case["bundle"])
                own = case.get("component_check")
                result = cc.check(
                    defaults["component"],
                    defaults["component_version"],
                    component_check=cc.ComponentCheck(
                        name=own["name"], result=own["result"], message=own["message"],
                        reason=own["reason"], expected=own["expected"]) if own else None,
                    own_hash=defaults["own_hash"],
                    own_version=defaults["own_version"],
                    bundle_file=str(bundle),
                    device_marker=str(marker),
                    force=case.get("force", False),
                )
                self.assertEqual(case["result"], result.result)
                self.assertEqual(case["reason"], result.reason)
                self.assertEqual(case["refuse"], result.refuse)
                self.assertEqual(case["expected_hash"], result.expected_hash)
                # The message names the real bundle path; the vectors use a fixed one.
                self.assertEqual(
                    case["message"], result.message.replace(str(bundle), defaults["bundle_file"])
                )


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "image").mkdir()
        self.bundle = self.dir / "bundle.json"

    def run_check(self, component="raccoon-lib", persistent=False, enforce=True, own="aaaa", text=None):
        if text is not None:
            self.bundle.write_text(text)
        return cc.startup_check(
            component, "v1", persistent=persistent, enforce=enforce,
            status_directory=str(self.dir / "run"), own_hash=own, own_version="v9",
            bundle_file=str(self.bundle), device_marker=str(self.dir / "image"),
        )

    def test_ok_writes_status_file(self):
        self.run_check(text='{"contract_hash":"aaaa","bundle":"b","components":' + VERSIONS + '}')
        document = json.loads((self.dir / "run" / "raccoon-lib.json").read_text())
        self.assertEqual("ok", document["result"])
        self.assertEqual(os.getpid(), document["pid"])
        self.assertFalse(document["persistent"])
        self.assertEqual([], [p.name for p in (self.dir / "run").glob(".*")])  # no temp leftovers

    def test_mismatch_raises_and_still_writes_status(self):
        with self.assertRaises(cc.ContractError) as raised:
            self.run_check(text='{"contract_hash":"bbbb","components":' + VERSIONS + '}')
        self.assertEqual("mismatch", raised.exception.result.result)
        self.assertEqual("mismatch", json.loads((self.dir / "run" / "raccoon-lib.json").read_text())["result"])

    def test_enforce_false_does_not_raise(self):
        result = self.run_check(text='{"contract_hash":"bbbb","components":' + VERSIONS + '}', enforce=False)
        self.assertTrue(result.refuse)

    def test_skipped_writes_nothing(self):
        result = cc.startup_check(
            "raccoon-lib", "v1", persistent=False, status_directory=str(self.dir / "run"),
            own_hash="aaaa", bundle_file=str(self.bundle), device_marker=str(self.dir / "nope"),
        )
        self.assertEqual("skipped", result.result)
        self.assertFalse((self.dir / "run").exists())

    def test_unwritable_status_dir_does_not_stop_the_component(self):
        blocker = self.dir / "blocker"
        blocker.write_text("a file, not a directory")
        self.bundle.write_text('{"contract_hash":"aaaa","components":' + VERSIONS + '}')
        result = cc.startup_check(
            "raccoon-lib", "v1", persistent=False, status_directory=str(blocker / "run"),
            own_hash="aaaa", bundle_file=str(self.bundle), device_marker=str(self.dir / "image"),
        )
        self.assertEqual("ok", result.result)

    def test_exit_code_is_reserved_value(self):
        result = self.run_check(text='{"contract_hash":"bbbb","components":' + VERSIONS + '}', enforce=False)
        with self.assertRaises(SystemExit) as raised:
            cc.exit_if_refused(result)
        self.assertEqual(78, raised.exception.code)


class AggregateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "image").mkdir()
        self.bundle = self.dir / "bundle.json"
        self.run_dir = self.dir / "run"

    def status(self, component, own, persistent, pid=None, starttime="same", component_check=None):
        self.bundle.write_text(self.bundle_text)
        result = cc.check(component, "v1", own_hash=own, own_version="v9", component_check=component_check,
                          bundle_file=str(self.bundle), device_marker=str(self.dir / "image"))
        path = cc.write_status(result, persistent=persistent, directory=str(self.run_dir))
        if pid is not None:
            document = json.loads(path.read_text())
            document["pid"] = pid
            if starttime != "same":
                document["pid_starttime"] = starttime
            path.write_text(json.dumps(document))

    def agg(self):
        return cc.aggregate(bundle_file=str(self.bundle), directory=str(self.run_dir),
                            device_marker=str(self.dir / "image"))

    def test_not_a_device_is_ok_and_empty(self):
        summary = cc.aggregate(bundle_file=str(self.bundle), directory=str(self.run_dir),
                               device_marker=str(self.dir / "nope"))
        self.assertTrue(summary["ok"])
        self.assertFalse(summary["on_device"])
        self.assertEqual([], summary["components"])
        self.assertIsNone(summary["bundle"]["error"])  # a missing bundle file is not an error off-device

    def test_forced_aggregate_on_a_non_device_reports_the_bundle_error(self):
        summary = cc.aggregate(bundle_file=str(self.bundle), directory=str(self.run_dir),
                               device_marker=str(self.dir / "nope"), force=True)
        self.assertFalse(summary["ok"])
        self.assertEqual("file-missing", summary["bundle"]["error"])

    bundle_text = json.dumps({"bundle": "b", "contract_hash": "aaaa", "components": {"raccoon-transport": "v9", **json.loads(VERSIONS)}})

    def test_all_ok(self):
        self.status("raccoon-cli", "aaaa", True)
        summary = self.agg()
        self.assertTrue(summary["ok"])
        self.assertEqual("aaaa", summary["expected_hash"])
        self.assertEqual("v9", summary["bundle"]["transport_version"])
        self.assertTrue(summary["components"][0]["active"])

    def test_active_mismatch_blocks(self):
        self.status("raccoon-lib", "zzzz", False)
        summary = self.agg()
        self.assertFalse(summary["ok"])
        self.assertEqual(["raccoon-lib"], summary["blocking"])
        self.assertIn("raccoon-lib", cc.refusal_message(summary))

    def test_dead_persistent_service_keeps_blocking(self):
        self.status("stm32-data-reader", "zzzz", True, pid=2**22 + 12345)
        summary = self.agg()
        self.assertFalse(summary["components"][0]["active"])
        self.assertEqual(["stm32-data-reader"], summary["blocking"])

    def test_dead_run_component_is_ignored(self):
        self.status("raccoon-lib", "zzzz", False, pid=2**22 + 12345)
        self.assertTrue(self.agg()["ok"])

    def test_reused_pid_with_other_starttime_is_not_active(self):
        self.status("stm32-data-reader", "aaaa", True, pid=os.getpid(), starttime=1)
        self.assertFalse(self.agg()["components"][0]["active"])

    def test_dev_mode_reports_differing_hashes(self):
        self.bundle_text = json.dumps({"contract_hash": "dev", "components": json.loads(VERSIONS)})
        self.status("a", "1111", True)
        self.status("b", "2222", True)
        summary = self.agg()
        self.assertTrue(summary["dev_mode"])
        self.assertTrue(summary["hashes_differ"])
        self.assertTrue(summary["ok"])  # dev mode never blocks

    def test_components_with_a_dev_component_check_are_listed(self):
        dev = cc.ComponentCheck(name="version", result="dev-warnung", message="omitted", reason="version-omitted")
        self.status("a", "aaaa", True, component_check=dev)
        self.status("b", "aaaa", True)
        summary = self.agg()
        self.assertEqual(["a"], summary["dev_components"])
        self.assertFalse(summary["dev_mode"])  # the contract itself is not in dev mode
        self.assertTrue(summary["ok"])

    def test_a_refusing_component_check_blocks_runs_and_is_named(self):
        bad = cc.ComponentCheck(name="version", result="version-mismatch", message="component version mismatch: a ...")
        self.status("a", "aaaa", True, component_check=bad)
        summary = self.agg()
        self.assertFalse(summary["ok"])
        self.assertEqual(["a"], summary["blocking"])
        self.assertIn("component version mismatch", cc.refusal_message(summary))
        self.assertEqual("version", summary["components"][0]["component_check"]["name"])

    def test_dead_run_component_with_dev_entry_is_not_listed(self):
        dev = cc.ComponentCheck(name="version", result="dev-warnung", message="omitted")
        self.status("raccoon-lib", "aaaa", False, pid=2**22 + 12345, component_check=dev)
        self.assertEqual([], self.agg()["dev_components"])

    def test_bundle_error_blocks(self):
        self.status("a", "aaaa", True)
        self.bundle.unlink()
        summary = self.agg()
        self.assertEqual("file-missing", summary["bundle"]["error"])
        self.assertIn("bundle", summary["blocking"])
        self.assertFalse(summary["ok"])

    def test_unreadable_status_file_is_reported(self):
        self.run_dir.mkdir()
        (self.run_dir / "broken.json").write_text("{nope")
        self.bundle.write_text(self.bundle_text)
        summary = self.agg()
        self.assertEqual("broken", summary["components"][0]["component"])
        self.assertFalse(summary["ok"])


if __name__ == "__main__":
    unittest.main()
