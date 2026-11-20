# Native SQL versus an imported logical plan

This offline benchmark compares two full ExecuteQueryRequest messages on the
native SQL frontend from [YDB PR #59453](https://github.com/ydb-platform/ydb/pull/59453).
The pair must have the same envelope and typed parameters. Use an explicit
catalog fixture containing the tables and column types referenced by both requests.

* SQL goes through the stock `PrepareGenericQuery` with `NATIVE_REQUIRED`.
  This includes the branch's unconditional rendering of the bound program with
  `NFrontend::Explain`. Unsupported SQL returns an error.
* `parsed_query.logical_plan` (tag 7) imports read-only operators and scalar
  expressions into the branch's native RBO representation. It checks catalog
  column types, expression references and typed parameters during import.

Both routes call `PrepareNativeQuery`. The server clock starts before protobuf
decoding and stops in `TKqpNewRBOTransformer::DoTransform` after `ComputeParents()`,
before statistics and optimization. Catalog fixture loading and actor setup are
outside the timer. The client clock covers packing and serializing a complete
request from its prepared SQL or protobuf payload. The two clocks measure these
intervals separately. RPC, physical compilation and execution are outside both.

## Build and run

```sh
dir=ydb/tests/tools/parsed_query_bench/skip_yql
./ya make --threads=10 --link-threads=1 --build relwithdebinfo -DNO_DEBUGINFO=yes -tA "$dir"
bench="$dir/parsed_query_skip_yql_bench"

"$bench" --catalog catalog.json --verify text.pb logical_plan.pb
"$bench" --catalog catalog.json --client-ready 10 100 text.pb logical_plan.pb > ready.csv
"$bench" --catalog catalog.json --bench 10 100 text.pb logical_plan.pb > server.csv
```

The integration tests in [ut/test_skip_yql.py](ut/test_skip_yql.py) contain a
complete parameterized SELECT, its catalog and the matching protobuf text.
The server benchmark verifies the native logical operations and ordered result
columns before warmup, then alternates which route runs first in each round.
A mismatch or unsupported input produces an error before timed rows are written.

For each stage, add the mean `ready_wall_ns` in `ready.csv` to the mean
`total_wall_ns` in `server.csv`. Dividing the SQL total by the logical-plan
total gives the estimated speedup for this branch. The CSV also records thread
CPU time and serialized request size.
