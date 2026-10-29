#!/usr/bin/env python3
"""Report frontend routing on the repository benchmark SQL and scalar schemas.

This measures parse/bind/profile acceptance, not execution correctness. The DDL
reader below is intentionally limited to these checked-in benchmark fixtures.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[7]
DATA = ROOT / "ydb/core/kqp/ut/rbo/data"


def catalog_arguments(suite):
    arguments = []
    schema = (DATA / "schema" / f"{suite}.sql").read_text()
    tables = re.findall(r"CREATE TABLE\s+`([^`]+)`\s*\((.*?)\n\);", schema, re.S)
    if not tables:
        raise ValueError(f"no table fixtures found for {suite}")
    for table, body in tables:
        columns = []
        for line in body.splitlines():
            line = line.split("--")[0].strip()
            column = re.match(r"([A-Za-z_][A-Za-z_0-9]*)\s+([A-Za-z_][A-Za-z_0-9]*(?:\(\d+,\s*\d+\))?)(.*)", line)
            if not column or column[1].upper() in ("PRIMARY", "INDEX", "UNIQUE", "CONSTRAINT"):
                continue
            kind = column[2].replace(" ", "")
            if kind == "Bytes":
                kind = "String"
            optional = "" if "NOT NULL" in column[3] else "?"
            columns.append(f"{column[1]}:{kind}{optional}")
        arguments.extend(("--table", table, ",".join(columns)))
    return arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=ROOT / "ydb/core/kqp/opt/rbo/frontend/cli/kqp_native_frontend")
    parser.add_argument("--output", type=Path, help="write per-query routing and diagnostics as JSON")
    parser.add_argument("--parser", choices=("grammar", "handwritten", "verify"), default="grammar")
    options = parser.parse_args()
    report = {"measurement": "frontend routing only; not execution coverage", "parser": options.parser, "suites": {}}
    errors = 0
    for suite in ("tpch", "tpcds", "clickbench"):
        arguments = catalog_arguments(suite)
        rows = []
        for path in sorted((DATA / f"yql-{suite}").glob("q*.yql")):
            result = subprocess.run([str(options.cli), "--mode", "auto", "--parser", options.parser, *arguments],
                                    input=path.read_text(), text=True, capture_output=True, check=False, timeout=60)
            route = "error" if result.returncode else "legacy" if result.stdout.startswith("compiler=legacy") else "native"
            diagnostic = (result.stderr or result.stdout).splitlines()[0] if route != "native" else ""
            rows.append({"query": path.name, "route": route, "diagnostic": diagnostic})
        counts = dict(Counter(row["route"] for row in rows))
        errors += counts.get("error", 0)
        report["suites"][suite] = {"counts": counts, "queries": rows}
        print(f"{suite}: {counts}")
    if options.output:
        options.output.write_text(json.dumps(report, indent=2) + "\n")
    return int(errors != 0)


if __name__ == "__main__":
    raise SystemExit(main())
