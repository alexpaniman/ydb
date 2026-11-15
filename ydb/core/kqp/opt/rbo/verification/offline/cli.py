"""No-build command line for portable imports and serial offline verification."""

from __future__ import annotations

import argparse
from dataclasses import fields
import json
from pathlib import Path
import sys

from .corpus import OfflineError, import_case, load_corpus, read_json
from .runner import Limits, compare_runs, run_corpus


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    add = commands.add_parser("import", help="copy a saved semantic pair or bundle into a durable corpus")
    add.add_argument("--corpus", type=Path, required=True)
    add.add_argument("--id", required=True)
    add.add_argument("--provenance", type=Path, required=True, help="JSON with export_kind and description; native origin also needs pinned revision/binary hash")
    add.add_argument("--scope", choices=("endpoint", "prefix", "pair", "bundle"), default="endpoint")
    for name in ("before", "after", "observation", "bundle"):
        add.add_argument(f"--{name}", type=Path)
    add.add_argument("--attachment", action="append", default=[], metavar="NAME=PATH")
    check = commands.add_parser("check", help="verify all corpus identities and saved bytes")
    check.add_argument("corpus", type=Path)
    run = commands.add_parser("run", help="run current Python source serially; never build native programs")
    run.add_argument("corpus", type=Path)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--solver", type=Path, required=True)
    run.add_argument("--verifier-root", type=Path, default=Path(__file__).resolve().parent.parent)
    run.add_argument("--case", action="append", dest="selected")
    run.add_argument("--emit-smt", action="store_true", help="optional canonical formulas; counts against output and wall budgets")
    defaults = Limits()
    for field in fields(Limits):
        run.add_argument("--" + field.name.replace("_", "-"), type=int, default=getattr(defaults, field.name))
    compare = commands.add_parser("compare", help="compare completed runs with identical captures and budgets")
    compare.add_argument("before", type=Path)
    compare.add_argument("after", type=Path)
    return result


def main(arguments=None):
    options = parser().parse_args(arguments)
    try:
        if options.command == "import":
            attachments = {}
            for value in options.attachment:
                name, separator, path = value.partition("=")
                if not separator or not path or name in attachments:
                    raise OfflineError("attachments require distinct NAME=PATH arguments")
                attachments[name] = Path(path)
            result = import_case(options.corpus, options.id, read_json(options.provenance), scope=options.scope,
                                 before=options.before, after=options.after, observation=options.observation,
                                 bundle=options.bundle, attachments=attachments)
        elif options.command == "check":
            corpus = load_corpus(options.corpus)
            result = {"status": "INTEGRITY_OK", "cases": len(corpus["cases"]),
                      "note": "Integrity is not semantic admission or a proof."}
        elif options.command == "run":
            limits = Limits(**{field.name: getattr(options, field.name) for field in fields(Limits)})
            result = run_corpus(options.corpus, options.output, options.solver, verifier_root=options.verifier_root,
                                limits=limits, selected=options.selected, emit_smt=options.emit_smt)
        else:
            result = compare_runs(options.before, options.after)
        print(json.dumps(result, sort_keys=True, indent=2))
        if options.command == "run":
            return 0 if set(result["counts"]) <= {"VERIFIED_BOUNDED"} else 1
        return 0
    except KeyboardInterrupt:
        print(json.dumps({"status": "INTERRUPTED", "reason": "offline process tree stopped; run has no completion marker"}), file=sys.stderr)
        return 130
    except (OfflineError, OSError, ValueError, TypeError, KeyError) as error:
        print(json.dumps({"status": "OFFLINE_ERROR", "reason": str(error)}, sort_keys=True), file=sys.stderr)
        return 2
