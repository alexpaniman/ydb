"""Immutable case imports, portable paths and content-based identities."""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile


FORMAT = "ydb-rbo-offline-corpus"
VERSION = 1
MAX_ARTIFACT_BYTES = 64 << 20
MAX_MANIFEST_BYTES = 16 << 20
SCOPES = frozenset({"endpoint", "prefix", "pair", "bundle"})
_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
_SHA = re.compile(r"[0-9a-f]{64}\Z")


class OfflineError(RuntimeError):
    pass


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"), allow_nan=False) + "\n").encode()


def fingerprint(value: object) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def digest(path: Path) -> str:
    with path.open("rb") as source:
        value = hashlib.sha256()
        for block in iter(lambda: source.read(1 << 20), b""):
            value.update(block)
        return value.hexdigest()


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise OfflineError(f"repeated JSON key {key!r}")
        result[key] = value
    return result


def _constant(value):
    raise OfflineError(f"non-finite JSON constant {value}")


def read_json(path: Path, limit: int = MAX_MANIFEST_BYTES) -> dict:
    if path.is_symlink() or not path.is_file():
        raise OfflineError(f"expected a regular, nonsymlink JSON file: {path}")
    if path.stat().st_size > limit:
        raise OfflineError(f"JSON file exceeds {limit} bytes: {path}")
    try:
        value = json.loads(path.read_bytes(), object_pairs_hook=_object, parse_constant=_constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise OfflineError(f"invalid JSON in {path}: {error}") from error
    if type(value) is not dict:
        raise OfflineError(f"expected a JSON object: {path}")
    return value


def atomic_json(path: Path, value: object) -> None:
    data = canonical(value)
    descriptor, temporary = tempfile.mkstemp(prefix=".write-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as target:
            target.write(data)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def _identifier(value: object, field: str) -> str:
    if not isinstance(value, str) or _ID.fullmatch(value) is None:
        raise OfflineError(f"{field} must be a short portable identifier")
    return value


def relative_file(root: Path, name: object) -> Path:
    if type(name) is not str or not name or "\\" in name or "\0" in name:
        raise OfflineError("artifact path must be a relative POSIX path")
    relative = PurePosixPath(name)
    if relative.is_absolute() or any(part in {"..", "."} for part in name.split("/")):
        raise OfflineError(f"artifact path escapes its directory: {name}")
    if str(relative) != name:
        raise OfflineError(f"artifact path is not canonical: {name}")
    path = root
    for part in relative.parts:
        path = path / part
        if path.is_symlink():
            raise OfflineError(f"artifact path contains a symlink: {name}")
    if not path.is_file():
        raise OfflineError(f"artifact is missing or not a regular file: {name}")
    return path


def _provenance(value: object) -> dict:
    if type(value) is not dict or value.get("export_kind") not in {"native", "synthetic", "unknown"}:
        raise OfflineError("provenance requires export_kind: native, synthetic, or unknown")
    if not isinstance(value.get("description"), str) or not value["description"].strip():
        raise OfflineError("provenance requires a nonempty description")
    if value["export_kind"] == "native":
        revision = value.get("source_revision")
        binary = value.get("capture_binary_sha256")
        if not isinstance(revision, str) or re.fullmatch(r"[0-9a-f]{40}", revision) is None:
            raise OfflineError("native provenance requires a pinned 40-character source_revision")
        if not isinstance(binary, str) or _SHA.fullmatch(binary) is None:
            raise OfflineError("native provenance requires capture_binary_sha256")
    # Provenance is supplied by the importer, not an attestation that a native
    # capture was executed here. Keep additional settings and adapter receipts.
    canonical(value)
    return value


def _artifact(path: Path, relative: str) -> dict:
    return {"path": relative, "sha256": digest(path), "bytes": path.stat().st_size}


def _copy(source: Path, destination: Path, *, json_object: bool = False) -> None:
    if source.is_symlink() or not source.is_file():
        raise OfflineError(f"input must be a regular, nonsymlink file: {source}")
    if source.stat().st_size > MAX_ARTIFACT_BYTES:
        raise OfflineError(f"input exceeds {MAX_ARTIFACT_BYTES} bytes: {source}")
    before = digest(source)
    shutil.copyfile(source, destination)
    if digest(source) != before or digest(destination) != before:
        raise OfflineError(f"input changed during import: {source}")
    if json_object:
        read_json(destination, MAX_ARTIFACT_BYTES)


def import_case(
    root: Path, case_id: str, provenance: dict, *, scope: str = "endpoint",
    before: Path | None = None, after: Path | None = None,
    observation: Path | None = None, bundle: Path | None = None,
    attachments: dict[str, Path] | None = None,
) -> dict:
    """Copy bytes, not parsed semantic objects; unsupported exports stay useful."""
    _identifier(case_id, "case id")
    _provenance(provenance)
    if scope not in SCOPES:
        raise OfflineError(f"unsupported comparison scope: {scope}")
    if scope == "bundle":
        if bundle is None or any(path is not None for path in (before, after, observation)):
            raise OfflineError("bundle scope requires only --bundle")
    elif before is None or after is None or bundle is not None or ((scope == "pair") != (observation is not None)):
        raise OfflineError("pair inputs require before/after; only pair scope requires observation")
    root = root.absolute()
    if root.is_symlink():
        raise OfflineError("corpus directory must not be a symlink")
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".import.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        manifest_path = root / "corpus.json"
        manifest = load_corpus(root) if manifest_path.exists() else {"format": FORMAT, "version": VERSION, "cases": []}
        if any(case["id"] == case_id for case in manifest["cases"]):
            raise OfflineError(f"case already exists: {case_id}")
        cases = root / "cases"
        if cases.is_symlink():
            raise OfflineError("cases directory must not be a symlink")
        cases.mkdir(exist_ok=True)
        destination = cases / case_id
        if destination.exists():
            raise OfflineError(f"case directory already exists: {destination}")
        temporary = Path(tempfile.mkdtemp(prefix=".import-", dir=cases))
        try:
            inputs = {}

            def add(role: str, source: Path, name: str) -> None:
                _copy(source, temporary / name, json_object=True)
                inputs[role] = _artifact(temporary / name, f"cases/{case_id}/{name}")

            if scope == "bundle":
                add("original_bundle", bundle, "original-bundle.json")
                raw = read_json(temporary / "original-bundle.json")
                if (set(raw) != {"format", "version", "observation", "results"}
                        or raw["format"] != "ydb-rbo-result-bundle"
                        or type(raw["version"]) is not int or raw["version"] != 1
                        or raw["observation"] != "buffered_tuple_or_error"
                        or type(raw["results"]) is not list or not raw["results"]):
                    raise OfflineError("expected a nonempty v1 buffered result bundle")
                normalized = []
                seen = {"before": set(), "after": set()}
                for index, pair in enumerate(raw["results"]):
                    if type(pair) is not dict or set(pair) != {"before", "after"}:
                        raise OfflineError("bundle result requires before and after paths")
                    item = {}
                    for side in ("before", "after"):
                        source = relative_file(bundle.parent, pair[side])
                        identity = source.resolve()
                        if identity in seen[side]:
                            raise OfflineError(f"bundle repeats a {side} root")
                        seen[side].add(identity)
                        name = f"result-{index:04d}-{side}.json"
                        add(f"result_{index:04d}_{side}", source, name)
                        item[side] = name
                    normalized.append(item)
                atomic_json(temporary / "bundle.json", {**raw, "results": normalized})
                inputs["bundle"] = _artifact(temporary / "bundle.json", f"cases/{case_id}/bundle.json")
            else:
                add("before", before, "before.json")
                add("after", after, "after.json")
                if observation is not None:
                    add("observation", observation, "observation.json")
            saved_attachments = {}
            for name, source in sorted((attachments or {}).items()):
                _identifier(name, "attachment name")
                filename = f"attachment-{name}"
                _copy(source, temporary / filename)
                saved_attachments[name] = _artifact(temporary / filename, f"cases/{case_id}/{filename}")
            case = {"id": case_id, "scope": scope, "provenance": provenance, "inputs": inputs, "attachments": saved_attachments}
            case["sha256"] = fingerprint(case)
            os.rename(temporary, destination)
            manifest["cases"] = sorted([*manifest["cases"], case], key=lambda item: item["id"])
            atomic_json(manifest_path, manifest)
            return case
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)


def load_corpus(root: Path) -> dict:
    manifest = read_json(root / "corpus.json")
    if (set(manifest) != {"format", "version", "cases"} or manifest["format"] != FORMAT
            or type(manifest["version"]) is not int or manifest["version"] != VERSION
            or type(manifest["cases"]) is not list):
        raise OfflineError("expected a v1 offline corpus")
    seen = set()
    for case in manifest["cases"]:
        if type(case) is not dict or set(case) != {"id", "scope", "provenance", "inputs", "attachments", "sha256"}:
            raise OfflineError("malformed corpus case")
        case_id = _identifier(case["id"], "case id")
        if case_id in seen:
            raise OfflineError(f"duplicate case id: {case_id}")
        seen.add(case_id)
        if case["scope"] not in SCOPES:
            raise OfflineError(f"invalid scope in {case_id}")
        _provenance(case["provenance"])
        expected = fingerprint({key: value for key, value in case.items() if key != "sha256"})
        if case["sha256"] != expected:
            raise OfflineError(f"case manifest hash mismatch: {case_id}")
        if type(case["inputs"]) is not dict or type(case["attachments"]) is not dict:
            raise OfflineError(f"invalid artifact maps in {case_id}")
        required = ({"bundle", "original_bundle"} if case["scope"] == "bundle"
                    else {"before", "after", *(["observation"] if case["scope"] == "pair" else [])})
        if not required.issubset(case["inputs"]):
            raise OfflineError(f"missing inputs in {case_id}")
        if case["scope"] != "bundle" and set(case["inputs"]) != required:
            raise OfflineError(f"unexpected inputs in {case_id}")
        validate_case_artifacts(root, case)
    return manifest


def validate_case_artifacts(root: Path, case: dict) -> None:
    """Recheck one accepted case, avoiding an all-corpus scan per replay."""
    case_id = case["id"]
    for artifact in [*case["inputs"].values(), *case["attachments"].values()]:
        if (type(artifact) is not dict or set(artifact) != {"path", "sha256", "bytes"}
                or not isinstance(artifact["sha256"], str) or _SHA.fullmatch(artifact["sha256"]) is None
                or type(artifact["path"]) is not str
                or type(artifact["bytes"]) is not int or not 0 <= artifact["bytes"] <= MAX_ARTIFACT_BYTES):
            raise OfflineError(f"malformed artifact in {case_id}")
        if not artifact["path"].startswith(f"cases/{case_id}/"):
            raise OfflineError(f"artifact belongs to another case: {case_id}")
        path = relative_file(root, artifact["path"])
        if path.stat().st_size != artifact["bytes"] or digest(path) != artifact["sha256"]:
            raise OfflineError(f"artifact hash/size mismatch: {artifact['path']}")
    if case["scope"] == "bundle":
        _validate_saved_bundle(root, case)


def _validate_saved_bundle(root: Path, case: dict) -> None:
    bundle_path = relative_file(root, case["inputs"]["bundle"]["path"])
    raw = read_json(bundle_path)
    expected = []
    if type(raw.get("results")) is not list or not raw["results"]:
        raise OfflineError("saved bundle requires results")
    roles = {"bundle", "original_bundle"}
    for index, pair in enumerate(raw["results"]):
        item = {}
        for side in ("before", "after"):
            role = f"result_{index:04d}_{side}"
            roles.add(role)
            artifact = case["inputs"].get(role)
            if artifact is None:
                raise OfflineError(f"missing bundle root {role}")
            name = Path(artifact["path"]).name
            if not isinstance(pair, dict) or pair.get(side) != name:
                raise OfflineError("bundle references an unpinned root")
            item[side] = name
        expected.append(item)
    if set(case["inputs"]) != roles or raw != {
        "format": "ydb-rbo-result-bundle", "version": 1,
        "observation": "buffered_tuple_or_error", "results": expected,
    }:
        raise OfflineError("saved bundle does not match pinned roots")
