from __future__ import annotations

import json
import os
from pathlib import Path
import py_compile
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from offline.corpus import OfflineError, atomic_json, digest, fingerprint, import_case, load_corpus
from offline.runner import Limits, _decode_verdict, compare_runs, run_corpus, source_identity


PROVENANCE = {"export_kind": "synthetic", "description": "Protocol test fixture, not a captured production plan."}


class OfflineWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.before = self.root / "before.json"
        self.after = self.root / "after.json"
        # Intentionally unsupported by today's parser: importing it must not
        # require building a model or weaken future parser admission.
        atomic_json(self.before, {"future_operator": {"id": 1}})
        atomic_json(self.after, {"future_operator": {"id": 2}})
        self.corpus = self.root / "corpus"

    def add(self, **kwargs):
        return import_case(self.corpus, "q1", PROVENANCE, before=self.before, after=self.after, **kwargs)

    def test_preserves_unsupported_bytes_and_relocates(self):
        case = self.add()
        self.assertEqual(case["inputs"]["before"]["sha256"], digest(self.before))
        moved = self.root / "relocated"
        shutil.move(self.corpus, moved)
        self.assertEqual(load_corpus(moved)["cases"], [case])

    def test_hash_detects_mutation_and_duplicate_id_does_not_overwrite(self):
        self.add()
        with self.assertRaisesRegex(OfflineError, "already exists"):
            self.add()
        (self.corpus / "cases/q1/after.json").write_text('{"changed":true}')
        with self.assertRaisesRegex(OfflineError, "hash/size mismatch"):
            load_corpus(self.corpus)

    def test_provenance_does_not_invent_native_revision(self):
        with self.assertRaisesRegex(OfflineError, "source_revision"):
            import_case(self.corpus, "q1", {"export_kind": "native", "description": "Missing evidence"},
                        before=self.before, after=self.after)
        self.assertFalse(self.corpus.exists())

    def test_requires_original_observation_for_pair(self):
        with self.assertRaisesRegex(OfflineError, "observation"):
            self.add(scope="pair")
        case = self.add(scope="pair", observation=self.before)
        self.assertIn("observation", case["inputs"])

    def test_rejects_invalid_json_and_duplicate_keys(self):
        self.before.write_text('{"op":"Scan", "op":"Other"}')
        with self.assertRaisesRegex(OfflineError, "repeated JSON key"):
            self.add()
        self.assertFalse((self.corpus / "corpus.json").exists())

    def test_bundle_retains_joint_slot_order_and_original_manifest(self):
        before2 = self.root / "before2.json"
        after2 = self.root / "after2.json"
        atomic_json(before2, {"slot": 2})
        atomic_json(after2, {"slot": 3})
        manifest = self.root / "bundle.json"
        raw = {"format": "ydb-rbo-result-bundle", "version": 1, "observation": "buffered_tuple_or_error",
               "results": [{"before": "before2.json", "after": "after2.json"},
                           {"before": "before.json", "after": "after.json"}]}
        atomic_json(manifest, raw)
        case = import_case(self.corpus, "multi", PROVENANCE, scope="bundle", bundle=manifest)
        self.assertEqual(case["inputs"]["original_bundle"]["sha256"], digest(manifest))
        self.assertEqual(case["inputs"]["result_0000_before"]["sha256"], digest(before2))
        self.assertEqual(case["inputs"]["result_0001_before"]["sha256"], digest(self.before))
        self.assertEqual(load_corpus(self.corpus)["cases"], [case])

    def test_bundle_cannot_follow_external_paths_or_repeat_roots(self):
        manifest = self.root / "bundle.json"
        raw = {"format": "ydb-rbo-result-bundle", "version": 1, "observation": "buffered_tuple_or_error",
               "results": [{"before": "../before.json", "after": "after.json"}]}
        atomic_json(manifest, raw)
        with self.assertRaisesRegex(OfflineError, "escapes"):
            import_case(self.corpus, "multi", PROVENANCE, scope="bundle", bundle=manifest)
        raw["results"] = [{"before": "before.json", "after": "after.json"}] * 2
        atomic_json(manifest, raw)
        with self.assertRaisesRegex(OfflineError, "repeats"):
            import_case(self.corpus, "multi", PROVENANCE, scope="bundle", bundle=manifest)

    def test_manifest_cannot_redirect_a_pinned_input(self):
        case = self.add()
        case["inputs"]["before"]["path"] = "cases/q1/../../before.json"
        case["sha256"] = fingerprint({k: v for k, v in case.items() if k != "sha256"})
        atomic_json(self.corpus / "corpus.json", {"format": "ydb-rbo-offline-corpus", "version": 1, "cases": [case]})
        with self.assertRaisesRegex(OfflineError, "escapes"):
            load_corpus(self.corpus)

    def test_bundle_cannot_reference_an_unpinned_snapshot(self):
        manifest = self.root / "bundle.json"
        atomic_json(manifest, {"format": "ydb-rbo-result-bundle", "version": 1,
                             "observation": "buffered_tuple_or_error",
                             "results": [{"before": "before.json", "after": "after.json"}]})
        case = import_case(self.corpus, "multi", PROVENANCE, scope="bundle", bundle=manifest)
        saved = self.corpus / case["inputs"]["bundle"]["path"]
        raw = json.loads(saved.read_text())
        raw["results"][0]["before"] = "original-bundle.json"
        atomic_json(saved, raw)
        case["inputs"]["bundle"].update(sha256=digest(saved), bytes=saved.stat().st_size)
        case["sha256"] = fingerprint({k: v for k, v in case.items() if k != "sha256"})
        atomic_json(self.corpus / "corpus.json", {"format": "ydb-rbo-offline-corpus", "version": 1, "cases": [case]})
        with self.assertRaisesRegex(OfflineError, "unpinned"):
            load_corpus(self.corpus)

    def fake_verifier(self, status="VERIFIED_BOUNDED", code=0, extra=""):
        source = self.root / "verifier"
        package = source / "rbo_verifier"
        package.mkdir(parents=True, exist_ok=True)
        (package / "__init__.py").write_text("")
        (package / "__main__.py").write_text(
            "import json,sys\n"
            + extra + "\n"
            + f"print(json.dumps({{'status':{status!r},'row_bound':2,'task_bound':2}}))\n"
            + f"sys.exit({code})\n"
        )
        return source

    @staticmethod
    def limits():
        return Limits(solver_ms=100, construction_ms=100, wall_seconds=2, host_reserve_mib=1,
                      data_mib=256, address_mib=512, tree_rss_mib=512)

    def run_fake(self, name, **kwargs):
        return run_corpus(self.corpus, self.root / name, Path(sys.executable),
                          verifier_root=self.root / "verifier", limits=self.limits(), **kwargs)

    def test_serial_run_binds_sources_and_compares_actual_receipts(self):
        self.add()
        source = self.fake_verifier(status="UNKNOWN", code=2)
        first = self.run_fake("first")
        self.assertEqual(first["counts"], {"UNKNOWN": 1})
        self.fake_verifier()
        second = self.run_fake("second")
        self.assertEqual(second["counts"], {"VERIFIED_BOUNDED": 1})
        comparison = compare_runs(self.root / "first", self.root / "second")
        self.assertEqual(comparison["new_proofs"], ["q1"])
        self.assertNotEqual(comparison["before_verifier_sha256"], comparison["after_verifier_sha256"])
        receipt = json.loads((self.root / "second/q1/receipt.json").read_text())
        self.assertEqual(receipt["provenance"]["export_kind"], "synthetic")
        self.assertEqual(receipt["verifier_sha256"], source_identity(source)["sha256"])

    def test_success_exit_cannot_promote_unknown(self):
        self.add()
        self.fake_verifier(status="UNKNOWN", code=0)
        self.assertEqual(self.run_fake("bad")["counts"], {"INFRA_ERROR": 1})

    def test_same_size_source_edit_cannot_reuse_stale_bytecode(self):
        self.add()
        source = self.fake_verifier()
        module = source / "rbo_verifier/__main__.py"
        original = module.read_bytes()
        metadata = module.stat()
        # Place the control cache beside the fake module even when this test
        # itself runs under dev.py's isolated cache namespace.
        cache = module.parent / "__pycache__" / f"__main__.{sys.implementation.cache_tag}.pyc"
        py_compile.compile(str(module), cfile=str(cache), doraise=True,
                           invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP)
        self.fake_verifier(status="UNKNOWN", code=2)
        changed = module.read_bytes()
        padding = len(original) - len(changed)
        self.assertGreater(padding, 0)
        module.write_bytes(changed + b"#" + b" " * (padding - 1))
        os.utime(module, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        self.assertEqual(module.stat().st_size, len(original))
        # Control: -B alone still loads the obsolete timestamp cache. Ensure
        # the fixture exercises that failure, rather than merely changing code.
        environment = {**os.environ, "PYTHONPATH": str(source)}
        environment.pop("PYTHONPYCACHEPREFIX", None)
        stale = subprocess.run([sys.executable, "-B", "-m", "rbo_verifier"],
                               env=environment, capture_output=True, text=True, timeout=2)
        self.assertEqual(json.loads(stale.stdout)["status"], "VERIFIED_BOUNDED")
        self.assertEqual(self.run_fake("fresh-source")["counts"], {"UNKNOWN": 1})
        self.assertFalse((self.root / "fresh-source/q1/pycache").exists())

    def test_process_failure_never_becomes_proof(self):
        self.add()
        self.fake_verifier(extra="raise RuntimeError('fixture failure')")
        self.assertEqual(self.run_fake("bad")["counts"], {"INFRA_ERROR": 1})

    def test_changed_source_invalidates_even_a_successful_process(self):
        self.add()
        source = self.fake_verifier()
        from offline import runner
        original = runner.guarded_process

        def mutate(*args, **kwargs):
            result = original(*args, **kwargs)
            (source / "rbo_verifier/__init__.py").write_text("# changed during run\n")
            return result

        with patch.object(runner, "guarded_process", side_effect=mutate):
            self.assertEqual(self.run_fake("bad")["counts"], {"INFRA_ERROR": 1})

    def test_guard_timeout_remains_inconclusive(self):
        case = self.add()
        process = {"stop_reason": "wall_timeout", "exit_code": 0}
        verdict = _decode_verdict(self.root, process, case, self.limits())
        self.assertEqual(verdict["status"], "UNKNOWN")
        self.assertEqual(verdict["phase"], "offline_guard")

    def test_real_wall_guard_prevents_late_success(self):
        self.add()
        self.fake_verifier(extra="import time; time.sleep(5)")
        summary = run_corpus(self.corpus, self.root / "timeout", Path(sys.executable),
                             verifier_root=self.root / "verifier",
                             limits=Limits(**{**self.limits().__dict__, "wall_seconds": 1}))
        self.assertEqual(summary["counts"], {"UNKNOWN": 1})
        receipt = json.loads((self.root / "timeout/q1/receipt.json").read_text())
        self.assertEqual(receipt["process"]["stop_reason"], "wall_timeout")
        self.assertLess(receipt["process"]["elapsed_seconds"], 3)

    def descendant_program(self):
        pidfile = self.root / "descendant.pid"
        code = ("import os,pathlib,signal,time; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                f"pathlib.Path({str(pidfile)!r}).write_text(str(os.getpid())); "
                "time.sleep(10)")
        return pidfile, code

    def assert_descendant_stopped(self, pid):
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            try:
                state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
            except FileNotFoundError:
                return
            if state == "Z":
                return
            time.sleep(0.02)
        self.fail("offline guard left a live solver descendant")

    @staticmethod
    def cleanup_descendant(pidfile):
        if pidfile.exists():
            try:
                os.kill(int(pidfile.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass

    def test_normal_driver_exit_kills_uncooperative_descendant(self):
        self.add()
        pidfile, child = self.descendant_program()
        self.addCleanup(self.cleanup_descendant, pidfile)
        self.fake_verifier(extra=(
            "import subprocess,time\n"
            f"subprocess.Popen([sys.executable, '-c', {child!r}])\n"
            "time.sleep(0.15)"
        ))
        result = self.run_fake("cleanup")
        self.assertEqual(result["counts"], {"VERIFIED_BOUNDED": 1})
        self.assertTrue(pidfile.exists())
        self.assert_descendant_stopped(int(pidfile.read_text()))

    def test_sigterm_kills_descendants_and_leaves_no_completion_marker(self):
        self.add()
        pidfile, child = self.descendant_program()
        self.addCleanup(self.cleanup_descendant, pidfile)
        source = self.fake_verifier(extra=(
            "import subprocess,time\n"
            f"subprocess.Popen([sys.executable, '-c', {child!r}])\n"
            "time.sleep(10)"
        ))
        output = self.root / "interrupted"
        command = [sys.executable, "-B", "-m", "offline", "run", str(self.corpus),
                   "--output", str(output), "--solver", sys.executable, "--verifier-root", str(source)]
        for key, value in self.limits().__dict__.items():
            command.extend(("--" + key.replace("_", "-"), str(value)))
        environment = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[2]),
                       "PYTHONDONTWRITEBYTECODE": "1"}
        logfile = self.root / "interrupted.log"
        with logfile.open("wb") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, env=environment)
        try:
            deadline = time.monotonic() + 2
            while not pidfile.exists() and time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.02)
            self.assertTrue(pidfile.exists(), logfile.read_text())
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=2)
            self.assertEqual(process.returncode, 130, logfile.read_text())
            self.assertFalse((output / "summary.json").exists())
            self.assert_descendant_stopped(int(pidfile.read_text()))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_scope_and_bounds_are_checked(self):
        case = self.add(scope="prefix")
        atomic_json(self.root / "stdout.txt", {"status": "VERIFIED_BOUNDED", "row_bound": 2, "task_bound": 2})
        (self.root / "stderr.txt").write_text("")
        with self.assertRaisesRegex(OfflineError, "scope"):
            _decode_verdict(self.root, {"exit_code": 0, "stop_reason": None}, case, self.limits())

    def test_compare_rejects_different_budgets_and_tampering(self):
        self.add()
        self.fake_verifier()
        self.run_fake("first")
        run_corpus(self.corpus, self.root / "second", Path(sys.executable),
                   verifier_root=self.root / "verifier", limits=Limits(**{**self.limits().__dict__, "solver_ms": 200}))
        with self.assertRaisesRegex(OfflineError, "budgets"):
            compare_runs(self.root / "first", self.root / "second")
        (self.root / "first/q1/receipt.json").write_text("{}")
        with self.assertRaisesRegex(OfflineError, "receipt hash"):
            compare_runs(self.root / "first", self.root / "first")

    def test_missing_completion_marker_cannot_be_compared(self):
        self.add()
        self.fake_verifier()
        self.run_fake("first")
        (self.root / "first/summary.json").unlink()
        with self.assertRaises(OfflineError):
            compare_runs(self.root / "first", self.root / "first")

    def test_adapter_source_changes_participate_in_identity(self):
        source = self.fake_verifier()
        old = source_identity(source)
        adapter = source / "native_adapter"
        adapter.mkdir()
        (adapter / "operator.py").write_text("# reviewed conversion\n")
        self.assertNotEqual(source_identity(source), old)


if __name__ == "__main__":
    unittest.main()
