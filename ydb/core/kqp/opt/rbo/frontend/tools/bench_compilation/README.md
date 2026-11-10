# Native compilation comparison

`run.py` compares immutable pre-migration and native `kqprun` binaries. Run from
the repository root, with a separate output directory for each candidate:

```bash
python3 ydb/core/kqp/opt/rbo/frontend/tools/bench_compilation/run.py \
  --before /path/to/pre-migration-kqprun --after /path/to/native-kqprun \
  --framework /path/to/compilation_bench \
  --viewer /path/to/violinoscope/violinoscope.py \
  --out /tmp/native-comparison --warmups 5 --repeats 3
```

For an alternating control between two native checkpoints, add `--before-native`.
Both variants then require native phases and zero fallback, and both expose phase
timings. The manifest records this mode; resume rejects a change in it.

Build binaries with the workspace's clang-balancer. Use an ordinary `ya make`
build to publish `kqprun`: a test build can leave the source-tree executable link
pointing at an earlier build. Copy each published binary, check its hash, and
retain its source revision and patch before collecting measurements. Do not
replace binaries while a comparison or its controls are running.

The default corpus contains all 6,813 1C queries, 22 TPC-H queries, 99 TPC-DS
queries, 43 ClickBench queries, and the tiny and medium 1C samples. The full 1C
corpus remains external; `--framework` points to its `queries/*.yql` and
`schema.yql`. TPC-H/TPC-DS/ClickBench use the repository SQL and column-store
schemas. The existing workload lambda prefix is preserved. Tiny/medium sample
paths are normalized to `/Root/`; query logic is unchanged. The default paths
for the external corpus and viewer match the original development workspace,
and both can be overridden.

Each batch creates a local database, performs untraced warmup loops, and then
records the measured loops in the same process. The driver alternates variant
order between batches and runs one variant at a time. Both query caches, optimizer
fallback, and the optional YQL physical-stage peephole are disabled. The native
variant additionally requires `NATIVE_REQUIRED`. Run without competing builds,
tests, or profiles.

Every request remains in the results, including pre-migration compilation
failures. Native failure, missing native trace phases, or fallback makes the
comparison fail. Warmup failures also fail the native comparison. Speed ratios
use per-query medians and only successful queries shared by both variants; the
coverage counts retain the rest. The preserved pre-migration binary exposes the
outer `Compile query` span, while native traces also expose individual phases.
Operator typing is included in RBO optimization and must not be added to it a
second time.

Frontend totals include parsing, table discovery, binding/scalar typing, native
plan formatting, and operator translation. Plan formatting was first measured
separately in candidate `7dfea86f68f`; earlier traces cannot isolate that cost.
Each measurement records which frontend phases were present so comparisons do
not silently treat missing instrumentation as measured zero work.

The manifest records binary, driver and layout hashes, input commands, source
snapshot, observations, and trace hashes. `--resume` validates batch signatures
and trace hashes, and preserves the original source snapshot. `--render-only`
recreates reports from an existing manifest. Use `--no-render` to collect traces
without immediately launching Violinoscope. `--limit` and `--workloads` are useful
for tooling checks; limited runs do not establish full-corpus coverage.

After collection, run controls with tracing disabled:

```bash
python3 ydb/core/kqp/opt/rbo/frontend/tools/bench_compilation/untraced.py \
  --comparison /tmp/native-comparison --out /tmp/native-comparison-untraced \
  --warmups 3 --repeats 3
```

Controls retain the same binaries, queries, schemas, and compiler configuration.
Their durations are whole request durations, not the traced compiler span, so
compare before/after ratios within each measurement rather than equating the two
absolute timings. They also retain every failure, and support `--resume`.

The full reports expose coverage differences. Produce a second violin report
with identical successful query cohorts on both sides:

```bash
python3 ydb/core/kqp/opt/rbo/frontend/tools/bench_compilation/paired.py \
  --comparison /tmp/native-comparison --out /tmp/native-comparison-paired
```

This removes a query from the paired report if any measured compilation failed
in either variant. The original traces and coverage report remain intact.
