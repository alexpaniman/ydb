"""Serial source-Python replay with resource limits and strict result receipts."""

from __future__ import annotations

from collections import Counter
from dataclasses import asdict, dataclass
import math
import os
from pathlib import Path
import platform
import resource
import signal
import subprocess
import sys
import time

from .corpus import OfflineError, atomic_json, digest, fingerprint, load_corpus, read_json, relative_file, validate_case_artifacts


MIB = 1 << 20
SCOPES = {
    "endpoint": None,
    "prefix": "OPTIMIZER_TRANSFORMATION_PREFIX",
    "pair": "OPTIMIZER_TRANSFORMATION_PAIR",
    "bundle": "BUFFERED_RESULT_BUNDLE",
}
EXITS = {"VERIFIED_BOUNDED": 0, "COUNTEREXAMPLE": 1, "SCHEMA_MISMATCH": 1,
         "UNKNOWN": 2, "UNSUPPORTED": 2, "SOLVER_ERROR": 2}


@dataclass(frozen=True)
class Limits:
    rows: int = 2
    solver_ms: int = 10_000
    construction_ms: int = 120_000
    wall_seconds: int = 150
    data_mib: int = 1024
    address_mib: int = 2048
    tree_rss_mib: int = 2048
    output_mib: int = 64
    host_reserve_mib: int = 16384

    def validate(self):
        values = asdict(self)
        if any(type(value) is not int for value in values.values()):
            raise OfflineError("all limits must be integers")
        if self.rows < 0 or any(value <= 0 for key, value in values.items() if key != "rows"):
            raise OfflineError("row bound must be nonnegative; resource limits must be positive")
        if self.data_mib > self.address_mib:
            raise OfflineError("data memory limit must not exceed address-space limit")
        if max(self.data_mib, self.address_mib, self.tree_rss_mib) > 4096:
            raise OfflineError("offline resource limits may not exceed 4096 MiB")
        if self.wall_seconds < math.ceil((self.construction_ms + self.solver_ms) / 1000):
            raise OfflineError("wall deadline must cover the construction and solver budgets")


def source_identity(root: Path) -> dict:
    """Hash source bytes, including adapters even before the CLI imports them."""
    if not (root / "rbo_verifier" / "__main__.py").is_file():
        raise OfflineError(f"verifier source root has no rbo_verifier/__main__.py: {root}")
    files = {}
    for directory in ("rbo_verifier", "native_adapter", "offline"):
        for path in sorted((root / directory).rglob("*.py")):
            if "tests" in path.relative_to(root / directory).parts:
                continue
            if path.is_symlink():
                raise OfflineError(f"source identity cannot follow a symlink: {path}")
            files[path.relative_to(root).as_posix()] = digest(path)
    return {"sha256": fingerprint(files), "files": files}


def _inspect(pid: int):
    try:
        stat = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        children = []
        for path in Path(f"/proc/{pid}/task").glob("*/children"):
            try:
                children.extend(int(value) for value in path.read_text().split())
            except (OSError, ValueError):
                pass
        return stat[19], int(stat[21]) * os.sysconf("SC_PAGE_SIZE"), children
    except (OSError, ValueError, IndexError):
        return None


def _available_memory() -> int:
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError):
        pass
    raise OfflineError("cannot read host available memory from /proc/meminfo")


def guarded_process(arguments: list[str], directory: Path, limits: Limits, environment: dict) -> dict:
    if sys.platform != "linux" or not hasattr(os, "sched_getaffinity"):
        raise OfflineError("bounded offline replay currently requires Linux")
    if _available_memory() < (limits.host_reserve_mib + limits.tree_rss_mib) * MIB:
        raise OfflineError("insufficient available memory for the configured host reserve and run limit")
    cpu = min(os.sched_getaffinity(0))
    requested = {resource.RLIMIT_DATA: limits.data_mib * MIB,
                 resource.RLIMIT_AS: limits.address_mib * MIB,
                 resource.RLIMIT_CORE: 0,
                 resource.RLIMIT_FSIZE: limits.output_mib * MIB,
                 resource.RLIMIT_CPU: limits.wall_seconds + 1}
    effective = {}
    for kind, limit in requested.items():
        _, inherited = resource.getrlimit(kind)
        effective[kind] = limit if inherited == resource.RLIM_INFINITY else min(limit, inherited)

    def child_limits():
        os.sched_setaffinity(0, {cpu})
        for kind, limit in effective.items():
            resource.setrlimit(kind, (limit, limit))

    start = time.monotonic()
    known = {}
    peak = 0
    stopped = None
    process = None
    previous_signals = {}

    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"offline run interrupted by signal {signum}")

    def stop():
        if process is None:
            return
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        for pid, identity in known.items():
            current = _inspect(pid)
            if current is not None and current[0] == identity:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    with (directory / "stdout.txt").open("wb") as stdout, (directory / "stderr.txt").open("wb") as stderr:
        try:
            for signum in (signal.SIGINT, signal.SIGTERM):
                previous_signals[signum] = signal.signal(signum, interrupted)
            process = subprocess.Popen(arguments, stdout=stdout, stderr=stderr, env=environment,
                                       cwd=directory, start_new_session=True, preexec_fn=child_limits)
            while process.poll() is None:
                pending = [process.pid, *known]
                seen = set()
                total = 0
                while pending:
                    pid = pending.pop()
                    if pid in seen:
                        continue
                    seen.add(pid)
                    current = _inspect(pid)
                    if current is None:
                        known.pop(pid, None)
                        continue
                    identity, rss, children = current
                    if pid in known and known[pid] != identity:
                        known.pop(pid, None)
                        continue
                    known[pid] = identity
                    total += rss
                    pending.extend(children)
                peak = max(peak, total)
                if time.monotonic() - start >= limits.wall_seconds:
                    stopped = "wall_timeout"
                elif total > limits.tree_rss_mib * MIB:
                    stopped = "tree_memory_limit"
                elif _available_memory() < limits.host_reserve_mib * MIB:
                    stopped = "host_memory_reserve"
                if stopped:
                    stop()
                    break
                time.sleep(0.05)
            process.wait()
        finally:
            stop()
            if process is not None:
                process.wait()
            for signum, handler in previous_signals.items():
                signal.signal(signum, handler)
    return {"exit_code": process.returncode, "elapsed_seconds": round(time.monotonic() - start, 6),
            "peak_tree_rss_bytes": peak, "stop_reason": stopped, "cpu": cpu,
            "effective_rlimits": {str(kind): value for kind, value in effective.items()}}


def _decode_verdict(directory: Path, process: dict, case: dict, limits: Limits) -> dict:
    if process["stop_reason"]:
        return {"status": "UNKNOWN", "phase": "offline_guard", "reason": process["stop_reason"],
                "row_bound": limits.rows, "task_bound": 2}
    if any((directory / name).stat().st_size > 1 << 20 for name in ("stdout.txt", "stderr.txt")):
        raise OfflineError("verifier output exceeds the 1 MiB verdict protocol limit")
    stdout = (directory / "stdout.txt").read_text().strip()
    source = directory / ("stdout.txt" if stdout else "stderr.txt")
    verdict = read_json(source, 1 << 20)
    status = verdict.get("status")
    if status not in EXITS or process["exit_code"] != EXITS[status]:
        raise OfflineError("verifier status and exit code disagree")
    bounded = status in {"VERIFIED_BOUNDED", "COUNTEREXAMPLE", "SCHEMA_MISMATCH", "UNKNOWN"}
    for name, expected in (("row_bound", limits.rows), ("task_bound", 2)):
        if (bounded or name in verdict) and (type(verdict.get(name)) is not int or verdict[name] != expected):
            raise OfflineError(f"verifier {name} does not match this run")
    if verdict.get("comparison_scope") != SCOPES[case["scope"]]:
        raise OfflineError("verifier comparison scope does not match this case")
    if verdict.get("phase") == "construction" and verdict.get("construction_timeout_ms") != limits.construction_ms:
        raise OfflineError("verifier construction budget does not match this run")
    if case["scope"] == "pair":
        expected = case["inputs"]["observation"]["sha256"]
        if (bounded or "observation_snapshot_sha256" in verdict) and verdict.get("observation_snapshot_sha256") != expected:
            raise OfflineError("pair proof does not identify its original observation snapshot")
        if status in {"VERIFIED_BOUNDED", "COUNTEREXAMPLE"} and verdict.get("observation_kind") not in {"bag", "sequence"}:
            raise OfflineError("pair proof does not establish its observation kind")
    return verdict


def run_corpus(root: Path, output: Path, solver: Path, *, verifier_root: Path, limits: Limits,
               selected: list[str] | None = None, emit_smt: bool = False) -> dict:
    limits.validate()
    root, output, verifier_root, solver = (path.resolve() for path in (root, output, verifier_root, solver))
    if output.exists():
        raise OfflineError("run output must be a new directory")
    if not solver.is_file() or not os.access(solver, os.X_OK):
        raise OfflineError("--solver must name an existing executable; offline runs never build or download it")
    manifest = load_corpus(root)
    manifest_hash = digest(root / "corpus.json")
    cases = manifest["cases"]
    if selected is not None:
        if len(set(selected)) != len(selected) or set(selected) - {case["id"] for case in cases}:
            raise OfflineError("case selection contains duplicates or unknown ids")
        cases = [case for case in cases if case["id"] in set(selected)]
    if not cases:
        raise OfflineError("no cases selected")
    source = source_identity(verifier_root)
    solver_hash = digest(solver)
    python_hash = digest(Path(sys.executable).resolve())
    identity = {"cases": {case["id"]: case["sha256"] for case in cases}, "limits": asdict(limits),
                "task_bound": 2, "emit_smt": emit_smt}
    configuration = {"format": "ydb-rbo-offline-run", "version": 1, "comparison": identity,
                     "comparison_sha256": fingerprint(identity), "verifier": source,
                     "solver": {"path": str(solver), "sha256": solver_hash},
                     "python": {"version": sys.version, "sha256": python_hash},
                     "host": {"node": platform.node(), "platform": platform.platform()},
                     "capture_provenance_is_user_supplied": True}
    output.mkdir(parents=True)
    atomic_json(output / "configuration.json", configuration)
    # Snapshot the selected portable manifest, while cases remain in the corpus.
    atomic_json(output / "inputs.json", {**manifest, "cases": cases})
    environment = {**os.environ, "PYTHONPATH": str(verifier_root), "PYTHONDONTWRITEBYTECODE": "1",
                   "PYTHONHASHSEED": "0", "OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1",
                   "MKL_NUM_THREADS": "1"}
    results = []
    for case in cases:
        directory = output / case["id"]
        directory.mkdir()
        # -B disables cache writes, but Python can still read a stale timestamp
        # .pyc after a same-size edit within the filesystem's time granularity.
        # An empty, case-private cache prefix forces source reads as well, so
        # the source hashes in this receipt describe the code actually run.
        arguments = [sys.executable, "-B", "-X", f"pycache_prefix={directory / 'pycache'}", "-m", "rbo_verifier"]
        inputs = case["inputs"]
        if case["scope"] == "bundle":
            arguments.extend(("--bundle", str(relative_file(root, inputs["bundle"]["path"]))))
        else:
            arguments.extend(str(relative_file(root, inputs[side]["path"])) for side in ("before", "after"))
            if case["scope"] in {"prefix", "pair"}:
                arguments.append(f"--diagnostic-transformation-{case['scope']}")
            if case["scope"] == "pair":
                arguments.extend(("--diagnostic-observation-snapshot", str(relative_file(root, inputs["observation"]["path"]))))
        arguments.extend(("--solver", str(solver), "--rows", str(limits.rows),
                          "--timeout-ms", str(limits.solver_ms), "--construction-timeout-ms", str(limits.construction_ms)))
        if emit_smt:
            arguments.extend(("--emit-smt", str(directory / "obligation.smt2")))
        atomic_json(directory / "command.json", arguments)
        process = None
        try:
            # Only this case is read by the child. Validate the manifest and
            # these artifacts before/after, rather than rehashing every other
            # case twice per query (quadratic work on a large corpus).
            validate_case_artifacts(root, case)
            if digest(root / "corpus.json") != manifest_hash or source_identity(verifier_root) != source or digest(solver) != solver_hash:
                raise OfflineError("inputs, verifier sources or solver changed during the run")
            process = guarded_process(arguments, directory, limits, environment)
            validate_case_artifacts(root, case)
            if digest(root / "corpus.json") != manifest_hash or source_identity(verifier_root) != source or digest(solver) != solver_hash:
                raise OfflineError("inputs, verifier sources or solver changed during the run")
            verdict = _decode_verdict(directory, process, case, limits)
        except (OSError, UnicodeError, OfflineError) as error:
            verdict = {"status": "INFRA_ERROR", "reason": str(error)}
        receipt = {"case_id": case["id"], "case_sha256": case["sha256"], "scope": case["scope"],
                   "provenance": case["provenance"], "inputs": inputs,
                   "verifier_sha256": source["sha256"], "solver_sha256": solver_hash,
                   "limits": asdict(limits), "task_bound": 2, "process": process, "verdict": verdict}
        atomic_json(directory / "receipt.json", receipt)
        results.append({"case_id": case["id"], "status": verdict["status"],
                        "receipt": f"{case['id']}/receipt.json", "receipt_sha256": digest(directory / "receipt.json")})
        # A final summary is a completion marker. Partial runs retain receipts
        # but cannot be compared as if every selected case completed.
    summary = {"format": "ydb-rbo-offline-summary", "version": 1,
               "configuration_sha256": digest(output / "configuration.json"),
               "counts": dict(sorted(Counter(result["status"] for result in results).items())), "results": results}
    atomic_json(output / "summary.json", summary)
    return summary


def _load_run(root: Path):
    configuration = read_json(root / "configuration.json")
    summary = read_json(root / "summary.json")
    if (configuration.get("format") != "ydb-rbo-offline-run" or configuration.get("version") != 1
            or summary.get("format") != "ydb-rbo-offline-summary" or summary.get("version") != 1
            or summary.get("configuration_sha256") != digest(root / "configuration.json")):
        raise OfflineError("invalid or changed run configuration")
    comparison = configuration.get("comparison")
    if type(comparison) is not dict or fingerprint(comparison) != configuration.get("comparison_sha256"):
        raise OfflineError("run comparison contract hash mismatch")
    receipts = {}
    results = summary.get("results")
    if type(results) is not list:
        raise OfflineError("run summary has no result list")
    for result in results:
        path = relative_file(root, result["receipt"])
        if digest(path) != result["receipt_sha256"]:
            raise OfflineError("run receipt hash mismatch")
        receipt = read_json(path)
        case_id = result["case_id"]
        if (case_id in receipts or receipt.get("case_id") != case_id
                or receipt.get("case_sha256") != comparison["cases"].get(case_id)
                or receipt.get("limits") != comparison["limits"]
                or receipt.get("task_bound") != comparison["task_bound"]
                or receipt.get("verdict", {}).get("status") != result["status"]
                or receipt.get("verifier_sha256") != configuration["verifier"]["sha256"]
                or receipt.get("solver_sha256") != configuration["solver"]["sha256"]):
            raise OfflineError("receipt does not match run identity")
        receipts[case_id] = receipt
    if set(receipts) != set(comparison["cases"]):
        raise OfflineError("run is incomplete or contains unexpected cases")
    return configuration, receipts


def compare_runs(before: Path, after: Path) -> dict:
    left, old = _load_run(before)
    right, new = _load_run(after)
    if left["comparison"] != right["comparison"]:
        raise OfflineError("runs use different cases, capture hashes, scopes, bounds or resource budgets")
    if left["solver"]["sha256"] != right["solver"]["sha256"] or left["python"] != right["python"]:
        raise OfflineError("runs use different solver or Python identities")
    rows = []
    for case_id in sorted(old):
        a, b = old[case_id], new[case_id]
        row = {"case_id": case_id, "before": a["verdict"]["status"], "after": b["verdict"]["status"],
               "before_seconds": a["process"]["elapsed_seconds"] if a["process"] else None,
               "after_seconds": b["process"]["elapsed_seconds"] if b["process"] else None}
        rows.append(row)
    return {"format": "ydb-rbo-offline-comparison", "version": 1,
            "before_verifier_sha256": left["verifier"]["sha256"], "after_verifier_sha256": right["verifier"]["sha256"],
            "same_recorded_host": left["host"] == right["host"],
            "same_effective_limits": all(
                old[key]["process"] is not None and new[key]["process"] is not None
                and old[key]["process"].get("effective_rlimits") == new[key]["process"].get("effective_rlimits")
                and old[key]["process"].get("cpu") == new[key]["process"].get("cpu") for key in old),
            "new_proofs": [r["case_id"] for r in rows if r["before"] != "VERIFIED_BOUNDED" and r["after"] == "VERIFIED_BOUNDED"],
            "lost_proofs": [r["case_id"] for r in rows if r["before"] == "VERIFIED_BOUNDED" and r["after"] != "VERIFIED_BOUNDED"],
            "results": rows}
