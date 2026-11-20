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
./ya make --threads=10 --link-threads=1 --build relwithdebinfo -DNO_DEBUGINFO=yes "$dir"
bench="$dir/parsed_query_skip_yql_bench"

"$bench" --catalog catalog.json --verify text.pb logical_plan.pb
"$bench" --catalog catalog.json --client-ready 10 100 text.pb logical_plan.pb > ready.csv
"$bench" --catalog catalog.json --bench 10 100 text.pb logical_plan.pb > server.csv
```

## Generate a compact request pair

Use the [NewParser benchmark producer](https://github.com/pavelvelikhov/ydb/pull/3)
to export SQL with its catalog and typed request envelope. The producer's
`--export` command writes all six stages, with the SQL and LogicalPlan requests
at the first and last output paths.

```sh
"$producer" --catalog catalog.json --template TEMPLATE.pb --export query.yql \
    text.pb antlr_ast.pb antlr_cst.pb script.pb native_query.pb source_plan.pb
```

For FDW cases use the captured SQL and LogicalPlan request bytes from the same
case. Generation works offline with either producer. It requires Python and
the `protobuf` package (4.22.5 or newer).

```sh
"$bench" --dump-descriptor > request.desc
python3 "$dir/generate.py" --descriptor request.desc text.pb source_plan.pb compact
"$bench" --catalog catalog.json --verify compact/text.pb compact/logical_plan.pb
```

The generator copies `text.pb` byte for byte and compares both execution
envelopes, including parameter types and values. It compacts v1 graphs with
unordered public read-only results, unit rows, scans, maps, filters, equijoins,
cross joins, COUNT and replicate ports. It removes type-preserving column alias
maps when projection bindings remain distinct and join key order is preserved.
It drops single-reader primary replicate ports, shares identical context-free
literal expressions and assigns dense IDs to the remaining graph members.
Result labels, order and indices come from the source plan. A shared producer
with multiple readers retains its ports and bindings. Unsupported graph features
fail generation.

Pass `--verify-with "$bench" --catalog catalog.json` to the generator to require
both the original and compact pairs to pass the receiver verification before
publishing the output directory. The separate `--verify` above also works when
generation and SQL admission are checked independently. Only verified pairs
can enter `--bench`.

## Check and interpret the results

The integration tests in [ut/test_skip_yql.py](ut/test_skip_yql.py) contain a
complete parameterized SELECT, its catalog and the matching protobuf text.
Run the tests with the same build command and the additional flag `-tA`.
The server benchmark verifies the native logical operations and ordered result
columns before warmup, then alternates which route runs first in each round.
A mismatch or unsupported input produces an error before timed rows are written.

For each stage, add the mean `ready_wall_ns` in `ready.csv` to the mean
`total_wall_ns` in `server.csv`. Dividing the SQL total by the logical-plan
total gives the estimated speedup for this branch. The CSV also records thread
CPU time and serialized request size.
