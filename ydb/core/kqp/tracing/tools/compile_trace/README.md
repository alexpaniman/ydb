# Compilation span analysis

Tools for measuring where query compilation spends its time, using the
compilation spans (`ydb/core/kqp/tracing/kqp_compile_trace.h`).

## Spans

Compilation spans are children of the `Compile query` span of the compile actor:

| Verbosity | Spans |
|-----------|-------|
| 10 (Detailed) | phases: parsing, IO and type annotation, provider optimization, KQL building, `Optimize (new RBO)`, `Rule-based optimization`, physical conversion, physical query compilation, explain plan |
| 14 (Diagnostic) | steps: RBO stages, physical conversion steps, OLAP programs |
| 15 (Trace) | applied RBO rules, recomputed plan properties, peephole of each stage |

Attributes:

| Attribute | Meaning |
|-----------|---------|
| `ydb.cpu_us` | CPU time of the compilation while the span was open, children included. It is counted only while the compilation runs, so waits for metadata or statistics do not add to it. |
| `thread.id` | thread that started the span; synchronous spans run on it entirely |
| `ydb.compile.iteration` | run number of a phase that the pipeline repeats after a restart |
| `ydb.compile.restarted`, `ydb.compile.restart_from` | the phase during which the pipeline restarted, and the phase it restarted from |
| `ydb.expr.nodes`, `ydb.expr.nodes_after` | size of the expression a phase or peephole stage works on (verbosity 14 and above) |
| `ydb.rbo.stage`, `ydb.rbo.rule`, `ydb.rbo.property` | RBO stage, applied rule and recomputed plan property |
| `ydb.rbo.rule_attempts`, `ydb.rbo.rules_applied`, `ydb.rbo.unapplied_rules_us` | rule attempts of an RBO stage and the time of those that did not apply |
| `ydb.cbo.relations` | number of relations of a CBO join tree |
| `ydb.olap.program_bytes` | size of a compiled OLAP program |

Metadata and statistics requests (`Load metadata`, `Load statistics`) and literal
executions (`Execute plan`) are children of the phase that issued them.
`Rule-based optimization` groups the RBO stages for each plan root, separately
from finalization, plan JSON generation and conversion to the physical plan.

## Collecting

The `CompileTraceBench` test in `ydb/core/kqp/ut/rbo` compiles the TPC-H and/or
TPC-DS queries with the new RBO at each verbosity level, on a fresh test server
per level. Every server first compiles all queries once to warm its caches, then
compiles each query again with a distinct text, so nothing comes from the
compile cache. The levels are rotated between repeats. Level 0 compiles without
tracing and gives the baseline for the tracing overhead.

```
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/rbo -F 'KqpRboYql::CompileTraceBench' \
    --test-param COMPILE_TRACE_BENCH=tpcds \
    --test-param COMPILE_TRACE_LEVELS=0,10,14,15 \
    --test-param COMPILE_TRACE_REPEATS=3 \
    --test-param COMPILE_TRACE_OUT=/abs/path/compile_traces.jsonl
```

Parameters:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `COMPILE_TRACE_BENCH` | (test is skipped) | `tpch`, `tpcds` or both, comma separated |
| `COMPILE_TRACE_LEVELS` | `0,10,14,15` | verbosity levels |
| `COMPILE_TRACE_REPEATS` | `3` | measurements of every query at every level |
| `COMPILE_TRACE_QUERIES` | all supported | query numbers, comma separated |
| `COMPILE_TRACE_COLUMN_STORE` | `1` | `0` for row tables |
| `COMPILE_TRACE_COLLECTOR` | none | OTLP gRPC endpoint (e.g. `grpc://localhost:4317` of Jaeger) to export the spans to as well |
| `COMPILE_TRACE_OUT` | `compile_traces.jsonl` in the test output directory | output file; runs are appended |

The output has a `run` line per compilation (client time, level, repeat, host
load average) and a `span` line per span.

Measurements on a busy machine are noisy: the report shows the run-to-run
variation and the load average, and differences below about twice the
variation are not meaningful.

## Analysing

`compile_trace.py` needs only Python 3:

```
compile_trace.py load compile_traces.jsonl -o traces.db    # also accepts Jaeger /api/traces/<id> exports
compile_trace.py report traces.db --level 15               # health checks and breakdowns, Markdown
compile_trace.py perfetto traces.db --level 15 -o traces.json   # open in https://ui.perfetto.dev
compile_trace.py folded traces.db --level 15 -o traces.folded   # flamegraph.pl, speedscope
compile_trace.py folded traces.db --unit 1000 -o ff.txt     # Firefox Profiler: one sample per millisecond
compile_trace.py diff before.db after.db --level 14         # what changed, by span
```

The report starts with health checks that tell how far the numbers can be
trusted: coverage of `Compile query` by its child spans, time no child span
names, spans that end after their parent, overlap of concurrent spans, spans in
error, CPU against wall time, outlier queries, run-to-run variation and the
tracing overhead against level 0. Then it lists the own time of each span (its
duration minus the time its children cover), callers of spans with several
parents, plan properties, applied rules, the slowest queries, time against
input size and the cost of repeated phases.

The database has a `spans_view` view (one row per span with its benchmark,
query, level, repeat, parent, ancestors, own time and attributes) for ad hoc
SQL, e.g. with `sqlite3`, Datasette or DuckDB.

Reloading an input file updates existing spans by trace/span ID and replaces
that file's run records, so an appended benchmark file can be imported again.
Overlapping JSON lines and Jaeger exports do not duplicate spans.

Reports average calls and times across independent runs of each query, then sum
those averages over the selected queries. The slowest-query table and `diff`
use per-query medians instead. A trace identifies one run, including all its
compilations, so appending benchmark invocations that reuse repeat numbers does
not inflate the totals. Queries are identified by both benchmark and query
number, including in `folded --by-query` output.

### CPU profile of a span

To see what a span spends its time on, record a CPU profile with the wall clock
the spans use, then keep the samples taken inside spans of a name. To run the
test binary directly, pass the parameters in a test context file:

```
cat > ctx.json <<EOF
{"runtime": {"source_root": "$PWD", "build_root": "$PWD", "work_path": "$PWD",
  "test_params": {"COMPILE_TRACE_BENCH": "tpcds", "COMPILE_TRACE_LEVELS": "15",
                  "COMPILE_TRACE_REPEATS": "1", "COMPILE_TRACE_OUT": "$PWD/compile_traces.jsonl"}}}
EOF
YA_TEST_CONTEXT_FILE=ctx.json perf record -k CLOCK_REALTIME -F 999 -g -o perf.data -- \
    ydb/core/kqp/ut/rbo/ydb-core-kqp-ut-rbo KqpRboYql::CompileTraceBench
perf script -i perf.data -F comm,tid,time,ip,sym > perf.txt
compile_trace.py load compile_traces.jsonl -o traces.db
compile_trace.py perf-filter traces.db --perf-script perf.txt --name 'Peephole stage' -o peephole.folded
```

Samples are matched by time and, when present, `thread.id`. Spans without a
thread ID are matched by time only, which can include unrelated concurrent
work. For asynchronous spans, `thread.id` identifies only the starting thread;
filter their synchronous child spans to capture work after thread migrations.

## Limitations

- Wilson takes a span's start from the wall clock and its end from CPU cycles
  elapsed since, so spans started at different times drift apart by tens of
  ppm: on compilations of several seconds a child may end up to ~0.2 ms after
  its parent. The report counts such spans; the Perfetto conversion cuts them
  at the parent's end and keeps the real duration in `real_dur_us`.
- A rule span is recorded after the rule has applied, so plan properties that
  a rule recomputes itself (CBO) are its siblings, overlapping it, not its
  children.
- Spans measure wall time; `ydb.cpu_us` and the CPU columns of the report tell
  waiting from work. Load on the host inflates both, see the run-to-run
  variation in the report.
