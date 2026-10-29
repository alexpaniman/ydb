#!/usr/bin/env python3
"""Compare warm native parse+bind cost on identical SQL and catalog inputs."""

import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess


ROOT = Path(__file__).resolve().parents[7]
SCHEMA = "id:Uint64,value:Int32?"
CASES = [
    ("projection_filter", "SELECT value + 1 FROM t WHERE value > 0", SCHEMA),
    ("group_sort", "SELECT id, SUM(value) AS total FROM t WHERE value > 0 GROUP BY id "
     "HAVING SUM(value) > 10 ORDER BY total DESC LIMIT 20", SCHEMA),
    ("join", "SELECT a.id, b.value FROM t AS a JOIN t AS b ON a.id = b.id "
     "WHERE a.value > 0 AND b.value > 0 LIMIT 10", SCHEMA),
    ("wide_100", "SELECT " + ", ".join(f"c{i} + 1 AS v{i}" for i in range(100)) + " FROM t",
     ",".join(f"c{i}:Int32" for i in range(100))),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=ROOT / "ydb/core/kqp/opt/rbo/frontend/cli/kqp_native_frontend")
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.iterations <= 0 or args.trials <= 0:
        parser.error("iterations and trials must be positive")
    report = {
        "measurement": "warm parse, bind, profile checks, allocation and destruction; excludes catalog I/O, RBO and backend",
        "trials": args.trials, "iterations_per_trial": args.iterations, "cases": [],
    }
    for name, sql, schema in CASES:
        command = [str(args.cli), "--table", "t", schema]
        subprocess.run(command + ["--parser", "verify"], input=sql, text=True,
                       capture_output=True, check=True, timeout=60)
        samples = {kind: [] for kind in ("grammar", "handwritten")}
        for trial in range(args.trials):
            order = ("grammar", "handwritten") if trial % 2 == 0 else ("handwritten", "grammar")
            for kind in order:
                result = subprocess.run(command + ["--parser", kind, "--bench", str(args.iterations)],
                                        input=sql, text=True, capture_output=True, check=True, timeout=60)
                samples[kind].append(float(re.search(r"parse_bind_us=([0-9.]+)", result.stdout)[1]))
        medians = {kind: statistics.median(values) for kind, values in samples.items()}
        ratio = medians["grammar"] / medians["handwritten"]
        report["cases"].append({"name": name, "sql": sql, "schema": schema, "bytes": len(sql.encode()),
                                "samples_us": samples, "median_us": medians, "ratio": ratio})
        print(f"{name}: grammar={medians['grammar']:.2f} us handwritten={medians['handwritten']:.2f} us ratio={ratio:.2f}", flush=True)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
