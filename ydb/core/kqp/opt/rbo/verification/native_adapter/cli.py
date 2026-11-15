"""Inspect or lower captured native structures without building native code."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from .archive import AdapterError, MAX_BYTES, load_archive, require
from .lower import SCOPE, lower


def source_identity():
    root = Path(__file__).resolve().parent.parent
    sources = {}
    for package in ("native_adapter", "rbo_verifier"):
        for path in sorted((root / package).rglob("*.py")):
            if "tests" in path.relative_to(root / package).parts:
                continue
            require(not path.is_symlink(), "adapter sources must not be symlinks")
            sources[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    encoded = json.dumps(sources, sort_keys=True, separators=(",", ":")).encode()
    return {"sha256": hashlib.sha256(encoded).hexdigest(), "files": sources}


def archive_digest(path):
    with path.open("rb") as source:
        data = source.read(MAX_BYTES + 1)
    require(len(data) <= MAX_BYTES, "archive byte budget exceeded")
    return hashlib.sha256(data).hexdigest()


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect", help="inventory captured structures, without claiming a proof")
    inspect.add_argument("input", type=Path)
    convert = commands.add_parser("lower", help="lower reviewed logical forms for diagnostic pair checks")
    convert.add_argument("input", type=Path)
    convert.add_argument("--output", type=Path, required=True)
    options = parser.parse_args(arguments)
    try:
        identity = source_identity()
        # Read a hash both before and after decoding so a changing input cannot
        # acquire a receipt for bytes different from those that were lowered.
        require(options.input.is_file() and not options.input.is_symlink(), "archive must be a regular file")
        require(options.input.stat().st_size <= MAX_BYTES, "archive byte budget exceeded")
        initial_hash = archive_digest(options.input)
        archive = load_archive(options.input)
        if options.command == "inspect":
            result = archive.inspect()
        else:
            output = options.output
            receipt = Path(str(output) + ".provenance.json")
            require(not output.exists() and not output.is_symlink() and not receipt.exists() and not receipt.is_symlink(),
                    "output and provenance must be new paths")
            snapshot = lower(archive)
            data = (json.dumps(snapshot, indent=2, sort_keys=True) + "\n").encode()
            result = {"format": "ydb-rbo-native-adaptation", "version": 1,
                      "status": "LOWERED_DIAGNOSTIC", "comparison_scope": SCOPE,
                      "source_archive_sha256": initial_hash, "semantic_snapshot_sha256": hashlib.sha256(data).hexdigest(),
                      "adapter_source": identity,
                      "capture_origin": "not established by adaptation; retain the native capture receipt",
                      "note": "Requires the original observation anchor for diagnostic pair verification; this receipt is not a proof."}
            require(archive_digest(options.input) == initial_hash,
                    "native archive changed during lowering")
            require(source_identity() == identity, "adapter source changed during lowering")
            with output.open("xb") as target:
                target.write(data)
            # Completion marker is last. An interrupted write without a receipt
            # is not a complete adaptation and must not be imported as one.
            try:
                with receipt.open("x") as target:
                    json.dump(result, target, sort_keys=True, indent=2)
                    target.write("\n")
            except BaseException:
                output.unlink()
                raise
        print(json.dumps(result, sort_keys=True, indent=2))
        return 0
    except (AdapterError, OSError, ValueError, KeyError, TypeError, RecursionError) as error:
        print(json.dumps({"status": "UNSUPPORTED_NATIVE_ADAPTATION", "reason": str(error)}, sort_keys=True), file=sys.stderr)
        return 2

