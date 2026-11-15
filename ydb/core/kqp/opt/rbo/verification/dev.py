#!/usr/bin/env python3
"""Run verifier Python tests directly, with one CPU and bounded resources."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[5]
sys.path[:0] = [str(HERE), str(REPOSITORY)]
# Direct script execution reads this entry point from source. Local imports
# must also avoid stale timestamp caches after rapid, same-size edits.
_IMPORT_CACHE = tempfile.TemporaryDirectory(prefix="rbo-dev-imports-")
sys.dont_write_bytecode = True
sys.pycache_prefix = _IMPORT_CACHE.name

from offline.corpus import OfflineError, atomic_json  # noqa: E402
from offline.runner import Limits, guarded_process  # noqa: E402


CORE_MODULES = (
    "test_ir", "test_parameters", "test_list_parameters", "test_stable_pickle",
    "test_join", "test_point_lookup", "test_unique_constraints",
)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--suite", choices=("workflow", "core", "all"), default="workflow",
                        help="workflow: offline/adapter tests; core: focused semantic tests; all: both plus all semantic tests")
    result.add_argument("--filter", action="append", help="unittest name glob; repeat to select multiple patterns")
    result.add_argument("--solver", type=Path, help="existing Z3 executable; defaults to RBO_Z3 if set")
    result.add_argument("--require-solver", action="store_true", help="fail instead of skipping solver tests when Z3 is absent")
    result.add_argument("--wall-seconds", type=int, default=120)
    result.add_argument("--artifacts", type=Path, help="new directory retaining process logs and a test receipt")
    result.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    return result


def run_tests(options):
    loader = unittest.TestLoader()
    if options.filter:
        loader.testNamePatterns = options.filter
    suite = unittest.TestSuite()
    if options.suite in {"workflow", "all"}:
        for package in ("offline", "native_adapter"):
            directory = HERE / package / "tests"
            if not directory.is_dir():
                raise OfflineError(f"test package is missing: {directory}")
            # Separate loaders avoid retaining a previous discovery top directory.
            discovery = unittest.TestLoader()
            discovery.testNamePatterns = loader.testNamePatterns
            suite.addTests(discovery.discover(str(directory), top_level_dir=str(HERE)))
    prefix = "ydb.core.kqp.opt.rbo.verification.ut."
    if options.suite == "core":
        suite.addTests(loader.loadTestsFromNames([prefix + name for name in CORE_MODULES]))
    elif options.suite == "all":
        suite.addTests(loader.loadTestsFromNames([
            prefix + path.stem for path in sorted((HERE / "ut").glob("test_*.py"))
        ]))
    if not suite.countTestCases():
        raise OfflineError("test selection is empty")
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    print(json.dumps({"tests": result.testsRun, "failures": len(result.failures),
                      "errors": len(result.errors), "skipped": len(result.skipped),
                      "successful": result.wasSuccessful(),
                      "solver_configured": bool(os.environ.get("RBO_Z3"))}), flush=True)
    return 0 if result.wasSuccessful() else 1


def main(arguments=None):
    options = parser().parse_args(arguments)
    try:
        if not 1 <= options.wall_seconds <= 1800:
            raise OfflineError("--wall-seconds must be between 1 and 1800")
        solver = options.solver or (Path(os.environ["RBO_Z3"]) if os.environ.get("RBO_Z3") else None)
        if solver is not None:
            solver = solver.resolve()
            if not solver.is_file() or not os.access(solver, os.X_OK):
                raise OfflineError("solver must be an existing executable; dev.py never builds or downloads one")
        elif options.require_solver:
            raise OfflineError("provide --solver or RBO_Z3; native builds are not needed")
        if options.child:
            return run_tests(options)

        def execute(directory):
            # -B alone still reads timestamp-based .pyc files. Use a fresh
            # cache location too, so quick same-size edits exercise source.
            command = [sys.executable, "-B", "-X", f"pycache_prefix={directory / 'bytecode'}",
                       str(Path(__file__).resolve()), "--child", "--suite", options.suite]
            for pattern in options.filter or ():
                command.extend(("--filter", pattern))
            environment = {**os.environ, "PYTHONPATH": os.pathsep.join((str(HERE), str(REPOSITORY))),
                           "PYTHONDONTWRITEBYTECODE": "1", "PYTHONHASHSEED": "0",
                           "OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1", "MKL_NUM_THREADS": "1"}
            if solver is not None:
                environment["RBO_Z3"] = str(solver)
            else:
                environment.pop("RBO_Z3", None)
            process = guarded_process(command, directory, Limits(wall_seconds=options.wall_seconds), environment)
            atomic_json(directory / "test-process.json", process)
            for name, stream in (("stdout.txt", sys.stdout), ("stderr.txt", sys.stderr)):
                path = directory / name
                if path.exists():
                    with path.open(errors="replace") as source:
                        for line in source:
                            stream.write(line)
            print(json.dumps({"process": process, "artifacts": str(directory) if options.artifacts else None}))
            return 0 if process["exit_code"] == 0 and process["stop_reason"] is None else 1

        if options.artifacts:
            options.artifacts.mkdir(parents=True, exist_ok=False)
            return execute(options.artifacts.resolve())
        with tempfile.TemporaryDirectory(prefix="rbo-python-tests-") as directory:
            return execute(Path(directory))
    except (OfflineError, OSError, ValueError) as error:
        print(f"Python test runner: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
