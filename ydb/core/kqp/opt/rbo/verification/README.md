# New RBO equivalence verifier

This standalone checker compares the logical Initial RBO snapshot with the
Final pre-physical StageGraph over one shared bounded database. A bounded
proof is about the accepted model, not unbounded SQL equivalence or runtime
execution. The exporter and every encoding strategy are part of the trusted path.

The default checks exact equality of modeled outcome languages. The explicit
`binary64_uf_universal_v1` mode instead proves the stronger sufficient condition
that every enabled initial/final schedule pair agrees under shared primitive
floating-point abstractions. Its `SAT` result is `UNKNOWN`, never a concrete
counterexample. See the [theorem](contracts/THEOREM.md) and the single
[assumptions card](contracts/EXTERNAL_ASSUMPTIONS.md) for the precise boundary.

Start with [the semantic contract](PLAN.md), [the audit map](TRUSTED_CORE.md),
[the findings and regression ledger](FINDINGS.md), or
[the benchmark runbook](BENCHMARK_COVERAGE.md). The
[historical archive](history/README.md) preserves the previous milestone journals;
historical test counts and workload outcomes are not current guarantees.

For a local visual walkthrough, open the standalone
[RBO Observatory](../verification_explorer/README.md): saved plan graphs,
operator inspection and counterexample traces, outside the proof-producing code.

## Develop from exports

The normal development loop uses saved exports and source Python. It does not
build native code or start YDB. Use the [offline corpus workflow](offline/README.md)
to import immutable initial/final snapshots, replay selected cases, and compare
proofs and timings across Python changes. The corpus preserves exact input bytes,
capture provenance, result-bundle order and diagnostic observation anchors.
Store real captures and runs in a persistent directory, outside temporary caches.

From the repository root, run focused tests with an existing solver:

```bash
python3 ydb/core/kqp/opt/rbo/verification/dev.py --suite core \
  --solver /existing/path/z3 --require-solver

python3 ydb/core/kqp/opt/rbo/verification/dev.py --suite workflow \
  --solver /existing/path/z3 --require-solver
```

`dev.py` uses one CPU and the offline runner's allocation, address-space,
process-tree RSS and wall-time limits. `--filter '*Name*'` narrows the test
selection; `--artifacts NEW_DIRECTORY` retains logs and a process receipt.
`--suite all` adds the full Python semantic suite. Without an explicit solver
or `RBO_Z3`, solver-dependent tests are reported as skipped; they are not proofs.
This command never invokes `ya` or downloads dependencies.

For structures rejected by the native semantic exporter, the optional
`--native-structure` capture sidecar preserves typed expression DAGs independently
of semantic admission. The [Python native adapter](native_adapter/README.md)
can inspect these archives and add reviewed scalar support without a native
rebuild. Its first lowering path covers logical Read/Filter/Map/Root plans and
is explicitly diagnostic. Incomplete operators, physical stages, subplans and
effects remain rejected; structural dumps do not constitute proofs. Existing
complete semantic endpoint snapshots retain the full ordinary verifier path.

Native builds are needed when changing capture fields, production optimizer
code, or the captured main revision. Batch those changes and recapture affected
cases once; Python encoding and solver iteration should reuse existing exports.

## Build and test

From the repository root. On this memory-constrained development host, use
[the Linux build guard](tools/run_limited.py) with `RBO_BUILD_CPUS=1`: it restricts
CPU affinity to one CPU, sets a hard 8 GiB data/allocation cap per process, and stops the process tree
when sampled aggregate RSS exceeds 16 GiB or host available memory falls below
16 GiB. The aggregate cutoff is monitored, not a kernel cgroup limit. Keep the
explicit worker limits too; `ya --memory-limit` alone is not a hard memory cap.
Virtual address space is capped separately at 64 GiB so the linker can map
read-only archives and debug sections; those mappings still count toward the
aggregate RSS guard when resident. Keep one build/link/test worker and reuse
the existing compilation cache. On this host, run native builds through
`~/projects/clang-balancer/balance` as well; its scheduler complements the hard
per-process limits and sampled tree guard. `YA_BUILD_THREADS=1` prevents the
balancer from supplying a larger automatic build-worker count.
For `native_cpp_ut` and the server-sized `integration_ut`, `benchmark_ut`, `prefix_capture/bin`, and `ydb/core/kqp/ut/rbo`
executables on Linux, add `-DRBO_LOW_MEMORY_LINK=yes`. This opt-in setting strips
debug sections only when linking these executables and disables their GDB
index. It reuses the `relwithdebinfo` objects; the resulting executables have
no source-level debug information. Normal builds retain their debug information.
The build guard also kills tracked descendants when the build driver exits, including children
that ignore termination. Its small process-lifecycle checks need no compiler or
large allocation:

```bash
python3 -m unittest ydb.core.kqp.opt.rbo.verification.tools.test_run_limited
```

Use `dev.py` above for Python tests. The local balancer currently supports
build-only `ya` invocations; do not pass test flags, including the combined
`-tA` spelling. Build the native executable first, then run selected tests
separately. This also avoids building the full native test suite's Z3/verifier
dependencies when only structural export fixtures are selected.

```bash
set -o pipefail
RBO_BUILD_CPUS=1 YA_BUILD_THREADS=1 python3 ydb/core/kqp/opt/rbo/verification/tools/run_limited.py \
  ~/projects/clang-balancer/balance ./ya make --build relwithdebinfo --threads=1 --link-threads=1 \
  --test-threads=1 --dist-store-threads=1 --memory-limit=48GB \
  -DRBO_LOW_MEMORY_LINK=yes \
  ydb/core/kqp/opt/rbo/verification/native_cpp_ut 2>&1 | tail -n 100

repository_root="$PWD"
mkdir -p /var/tmp/rbo-native-fixtures/testing_out_stuff
(
  cd /var/tmp/rbo-native-fixtures
  RBO_BUILD_CPUS=1 python3 "$repository_root/ydb/core/kqp/opt/rbo/verification/tools/run_limited.py" \
    "$repository_root/ydb/core/kqp/opt/rbo/verification/native_cpp_ut/ydb-core-kqp-opt-rbo-verification-native_cpp_ut" \
    'TNativeSemanticSnapshot::NativeStructure*' 2>&1 | tail -n 100
)
```

The conformance fixture writes filtered/unfiltered native and semantic JSON pairs
to `testing_out_stuff/native-structure-conformance-*.{native,semantic}.json`.
Keep these exports and their native source/binary identities for subsequent
Python adapter checks. The remaining native tests may require additional test
data, verifier executables and Z3; the structural-only filter does not run them.

Tests include the build; add `-F '*test-filter*'` for a focused run.
`native_cpp_ut` uses main's numeric column identities and current operator API.
The older `cpp_ut` fixtures still require migration from the previous API;
they remain in the recursive target and currently prevent a successful full
recursive C++ build. Passing the focused target does not resolve that gap.
The recursive verification target also includes inspector, replay, confirmation,
localization, prefix-capture, and benchmark tests. The intentionally failing
[Decimal runtime diagnostic](runtime_ut/README.md) is manual and not recursed.
Pure coverage-policy checks also have a lightweight
[benchmark_policy/ut](benchmark_policy/ut) target without the real KQP host or Z3.
Use `set -o pipefail` when the shell must retain the build/test exit status.

The Python kernel has no package dependencies. Hermetic `ya` tests use the
pinned `contrib/tools/z3` executable, not an ambient solver. Lightweight
`python3 -m unittest discover -s ydb/core/kqp/opt/rbo/verification/ut`
skips solver cases unless `RBO_Z3` names a compatible executable.

## Verify and inspect

```bash
PYTHONPATH=ydb/core/kqp/opt/rbo/verification \
python3 -m rbo_verifier initial.json final.json \
  --rows 2 --timeout-ms 10000 --construction-timeout-ms 120000 --solver /path/to/z3 \
  --emit-smt obligation.smt2
```

`--construction-timeout-ms` separately limits snapshot loading, model construction,
and canonical rendering requested by `--emit-smt`. It requires POSIX interval
timers on the main thread and never replaces an existing timer. Expiry returns
`UNKNOWN` with `phase: construction`, before writing a formula or starting a
solver. The benchmark uses a fixed 120-second construction deadline in addition
to its solver budget and records it in each corpus report. This resource limit
does not change the required query lists or proof bounds.

The verdict includes the row bound and fixed task bound of two. Without
`--solver`, successful construction returns `FORMULA_EMITTED`, not a proof.
`--emit-smt` writes the canonical obligation, not the actual internal solver
portfolio. See the [theorem and solver protocol](contracts/THEOREM.md), especially
the mandatory model-domain exclusions for checked-String totality, exact native
scalar cardinality guards, and integral AVG: standalone `SAT` of the raw
disjunction is not a counterexample. A scalar guard is projected away only after
proving its physical count cannot exceed one on any reachable input at the bound.

For multiple buffered results, pass `--bundle bundle.json` instead of the two
snapshot paths. Its strict manifest lists result slots in client-visible order:

```json
{"format":"ydb-rbo-result-bundle","version":1,
 "observation":"buffered_tuple_or_error",
 "results":[{"before":"r0.initial.json","after":"r0.final.json"},
            {"before":"r1.initial.json","after":"r1.final.json"}]}
```

Paths are relative to the manifest. The checker compares the joint result tuple
over one shared database, not independent per-result proofs; any slot error is
one buffered query error. Streaming prefixes and effects are excluded. If any
root requests binary64 mode, it applies to every slot and both sides and is
recorded in the verdict. Bundle proofs do not enable multi-result runtime replay.

### Check whether a bound exercises nonempty results

Add `--diagnose-nonempty-output --diagnostic-timeout-ms 10000` to a solver
invocation. After verification, a separate diagnostic asks whether each side can
produce a successful nonempty result at the same bound. Its own shared deadline,
errors and `SAT`/`UNSAT`/`UNKNOWN` answers never change the equivalence verdict.

`SAT` means modeled reachability, not a runtime witness: opaque or floating
abstractions may admit extra behaviors. `UNSAT` means no successful nonempty
execution; it does not distinguish empty results from unavoidable errors.
Model-domain exclusions must first be ruled out. For bundles, all result slots
must succeed and at least one must contain a row; before and after may use
different databases. This checks neither every branch nor mutation sensitivity.

A passing proof can therefore have two `UNSAT` diagnostics. For example, TPCDS
q8's `HAVING COUNT(*) > 10` cannot produce a group at two rows per table.
Use targeted larger bounds or small semantic mutation checks to exercise such
behavior; do not weaken the original query or count reachability as a proof.

### Inspect a candidate

```bash
ydb/core/kqp/opt/rbo/verification/inspect_bin/kqp_rbo_inspect plan final.json

ydb/core/kqp/opt/rbo/verification/inspect_bin/kqp_rbo_inspect \
  witness initial.json final.json --query exact-query.yql \
  --verifier-verdict saved-verdict.json --rows 2 --solver /path/to/z3
```

The saved verdict pins the exact candidate database; the inspector may solve
routing and bounded choices but cannot silently substitute another database.
Trace digests bind normalized snapshots and the exact query. They are
producer-revision-specific, not a promise of cross-revision renderer stability.

## Confirm a candidate

Every coverage `COUNTEREXAMPLE` requires explicit confirmation on isolated YDB
targets; this is separate from recursive tests because replay creates and
retains namespaces on those targets.

```bash
ydb/core/kqp/opt/rbo/verification/confirm_bin/kqp_rbo_confirm \
  /path/to/tpcds_coverage.json \
  --inspector /path/to/kqp_rbo_inspect --solver /path/to/z3 \
  --replay /path/to/kqp_rbo_replay --ydb /path/to/ydb \
  --artifacts /new/path/confirmation \
  --baseline-endpoint grpc://baseline-host:2136 --baseline-database /Root/baseline \
  --candidate-endpoint grpc://candidate-host:2136 --candidate-database /Root/candidate
```

Use matching configurations except for legacy versus new RBO, with new-RBO
fallback disabled. Supply the exact captured query, including benchmark
compatibility definitions. Confirmation validates SHA-bound inputs and processes
every candidate. Missing evidence, nondeterminism, multi-result queries, or child
protocol failures produce `UNRESOLVED`, never a proof.

`REAL_RESULT_DIVERGENCE` means real executions differed; `NOT_REPRODUCED`
does not prove equivalence. Replay records `trace_plan_reproduced:false`:
the reduced catalog and external CLI do not establish that the exact supplied
StageGraph ran. Replay never deletes existing or generated YDB objects.
Detailed replay mechanics and the retained original invocation are in the
[archived replay section](history/pre-audit-refactor/README.md#real-ydb-counterexample-replay).

## Localize and preserve findings

The [transformation localizer](tools/README.md) checks both halves of each
captured interval, then descends to every adjacent step. Equivalent endpoints
do not prune an interval: transformations can cancel. Its explicit
`OPTIMIZER_TRANSFORMATION_PAIR` scope is diagnostic, not a normal Initial/Final
proof. The optional sequential strategy retains the earlier first-prefix scan.

For exhaustive audits, `--strategy adjacent --all-steps` checks the initial/final
pair and every adjacent boundary, including the final suffix. It omits coarse
interval proofs while retaining the same per-step coverage and snapshot guards.
For N nonzero events this requires N+2 proof calls, compared with 2N+1 for the
default midpoint schedule. Export failures and solver timeouts remain coverage
gaps; neither schedule attributes them to optimizer bugs.

Preserve the exact query, snapshots, raw verdict, formula, inspector trace,
confirmation streams, and relevant prefix captures. A model error and a runtime
divergence can coexist; retain a regression for each established cause.
Keep verifier/model changes separate from optimizer fixes; land an optimizer
fix with its focused regression. Change measured coverage claims only after
the corresponding complete rerun. No intentionally failing commit is needed
solely to preserve a finding.
