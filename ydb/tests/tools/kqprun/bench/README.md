# Compilation traces

From the repository root, build once and collect:

```sh
./ya make --build relwithdebinfo ydb/tests/tools/kqprun
python3 ydb/tests/tools/kqprun/bench/collect.py --out /tmp/compile-no-peephole --no-peephole
```

Open `/tmp/compile-no-peephole/violinoscope.html`. It is self-contained and starts with the supplied compilation layout. Nothing needs to be served.

The helper expects the external 1C corpus at `~/projects/optimizer-test-framework/compilation_bench` and the viewer at `~/projects/violinoscope/violinoscope.py`. Use `--framework PATH` and `--viewer PATH` to change those. The other workloads come from this checkout and the small repros beside the script.

For a quick check, add `--limit 3`. Use `--workloads tpch,tpcds` to select workloads, `--batch-size 1000` to control database restarts, and `--resume` to reuse completed batches. Resume checks the binary and input hashes; it refuses to mix changed configurations. Failed EXPLAIN requests are recorded and collection continues. A crash or schema failure stops the helper and identifies its log.

Without `--no-peephole`, the same command enables the pass. The switch specifically sets `TableServiceConfig.EnableNewRBOPhysicalStagePeephole=false`: it disables the new RBO physical-stage peephole, not every simplification pass elsewhere in the compiler.

| Workload | Included queries |
| --- | --- |
| 1C | Every numbered `.yql` file in the external corpus (6,813 in the recovered benchmark) |
| TPC-H | `yql-tpch/q1.yql` through `q22.yql`, column-store schema |
| TPC-DS | `yql-tpcds/q1.yql` through `q99.yql`, column-store schema |
| 1C medium | The recovered YQL-compatible repro in `samples/medium` |
| 1C tiny | `SELECT 1 FROM table LIMIT 1`, in `samples/tiny` |

Each query is explained once to warm metadata and once for measurement. Warm-up traces are excluded from the report. No table data is needed. New RBO is enabled, optimizer fallback is disabled, statistics collection is disabled, and the latest language version in this checkout is used. Trace level 15 preserves phase, stage and rule spans, matching the previous detailed report. The medium/tiny table path prefixes are flattened to `/Root/` to avoid separate directory setup.

`manifest.json` records the source commit, binary hash, exact configuration, commands and batch results. Each batch keeps its SQL inputs, OTLP `spans.jsonl`, server log and runner log. Failed compilations appear in the report's failure list and are excluded from successful-duration distributions. Counts need not equal an older report if compiler behavior changed.

## Just collect your own queries

```sh
ydb/tests/tools/kqprun/kqprun \
  --app-config ydb/tests/tools/kqprun/configuration/app_config.conf \
  --scheme-query schema.sql --script-query-dir queries \
  -C query -A explain --continue-after-fail \
  --trace-file spans.jsonl --trace-dataset experiment --trace-level 15
python3 ~/projects/violinoscope/violinoscope.py spans.jsonl --unit 'Compile query'
```

`--trace-file` overwrites the destination and streams complete OTLP JSON lines rather than retaining all spans in memory. Normal finalization allows 300 ms for spans ending after the query response, then checks the writer. The option traces query/script requests, not schema creation. `--trace-id` remains the existing text correlation ID; it does not enable span collection.

## Collect from a cluster

The cluster must run a build containing the compilation instrumentation. Configuring OpenTelemetry alone cannot add phases missing from the server binary.

Configure an OpenTelemetry Collector with an OTLP/gRPC receiver and JSON file exporter, as described in Violinoscope's `docs/collecting.md`. Then configure the server tracing backend and a scoped sampling rule. For example, this `TAppConfig` fragment records Query Service requests for a dedicated benchmark database:

```text
TracingConfig {
  Backend {
    Opentelemetry {
      CollectorUrl: "http://collector:4317"
      ServiceName: "compilation-benchmark"
    }
  }
  Sampling {
    Scope { Database: "/Root/benchmark" RequestTypes: "Query.ExecuteQuery" }
    Fraction: 1
    Level: 15
    MaxTracesPerMinute: 100000
    MaxTracesBurst: 10000
  }
}
```

Apply it using the cluster's normal configuration/deployment process on the participating nodes. Substitute the actual database, reachable Collector address and existing authentication configuration. Keep the scope restricted to the benchmark workload. Table Service callers use a different request type, for example `Table.ExplainDataQuery`.

Run the schema and EXPLAIN workload through your normal cluster client. Use the same table settings and warm-up sequence for comparable runs. The local helper itself runs kqprun; it is not a cluster driver. To select individual requests instead of sampling the database, configure `ExternalThrottling` with the same scope/level/rate fields and supply a valid W3C `traceparent` header. The C++ SDK exposes this as request settings `.TraceParent(...)`; `.TraceId(...)` is only a text correlation ID.

Once the workload and exporter have flushed, copy the Collector JSON files and build:

```sh
python3 ~/projects/violinoscope/violinoscope.py cluster=spans.jsonl \
  --unit 'Compile query' --layout ydb/tests/tools/kqprun/bench/compilation.layout.json \
  -o cluster.html
```

You can also export full traces from Jaeger or Tempo; the viewer's collection guide gives both routes. The viewer does not require either backend. These cluster instructions are checked against this checkout's tracing configuration and SDK; no live cluster was deployed as part of this local benchmark.
