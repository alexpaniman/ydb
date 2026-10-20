# Findings and focused regressions

This is the maintained status ledger, not a count of failing tests. A historical
counterexample, a deliberately mutated plan, and a current runtime divergence
are different evidence. The [archived finding inventory](history/pre-audit-refactor/BENCHMARK_COVERAGE.md#optimizer-correctness-findings)
retains the original observations and fix commits; do not read its milestone
statuses as fresh results.

## Native representation audit (2026-10-20, in progress)

The current audit is pinned to fetched main
`28f390e18e92099a29444d74faf294cd3a2f4c00`. The fresh census below covers
all 290 selected queries at two rows per table, two tasks and a 10-second solver
budget, with a separate 120-second construction deadline. Sources stayed fixed
through all eleven batches, and each batch's snapshots, formulas, verdicts,
process diagnostics and source hashes were archived before the next batch.
Full TPCH/TPCDS coverage remains an open goal.

| Suite | Queries | Prepared | Verified bounded | Unknown | Unsupported | Preparation failed | Counterexample |
|---|---:|---:|---:|---:|---:|---:|---:|
| TPCH | 22 | 22 | 12 | 8 | 2 | 0 | 0 |
| TPCDS | 99 | 92 | 20 | 50 | 20 | 7 | 2 |
| CLICKBENCH | 43 | 39 | 29 | 1 | 9 | 4 | 0 |
| JOB | 113 | 113 | 52 | 41 | 20 | 0 | 0 |
| SSB | 13 | 13 | 6 | 7 | 0 | 0 | 0 |

The subsequent source-bound R2 batch rechecked affected queries after exact
Boolean coalesce, Utf8 window keys, Date/interval admission and Int32 ranges.
TPCDS q10 and q45 changed from counterexample to UNKNOWN at the same 10-second
limit; q72 and q89 also now reach UNKNOWN. No counterexample was reported in
these targeted reruns. This does not refresh the whole census above.
Sixteen TPCDS queries now share one remaining exporter gap: a pushed
`Coalesce(CAST(text AS Date) comparison constant, false)` predicate. TPCH q12
and q19 require the complete Utf8-to-String SafeCast membership spelling.
TPCDS q51 still lacks the native ordered-ROWS bridge; q84 requires checked
Concat copies that retain their original column. JOB range failures reveal
additional unaudited range shapes and predicates on pruned read columns.

All seven shared-operator TPCDS queries prepare successfully in generic-query
mode with spilling enabled; data mode and generic mode without spilling still
fail. q14 and q39 produce two result roots. The next batch uses the bundle
verifier and records the query mode explicitly rather than treating successful
preparation as an equivalence proof. The default proof floor remains fixed to
data mode and ignores exploratory environment overrides.

The next applied changes cover the narrow pushed Date predicate, complete
SafeCast membership and checked Concat copy lineage. Their native/fresh-query
validation is pending. The copy-lineage changes passed all 934 Python tests in
isolation. The localizer now reports `LOCALIZATION_INCONSISTENT` if an endpoint
or coarse-interval failure contradicts a complete chain of adjacent bounded
proofs; its 28 isolated tests pass. This is a reporting repair, not a newly
confirmed optimizer defect. The 45 focused representation cases and broad
adjacent-transformation audits remain to be completed.

### Production findings

Each fix has its own branch with the pinned main as its direct parent. These
commits have not been pushed. The six metadata/template/reporting fixes pass
their focused tests on the combined SMT branch. The new-RBO scalar-domain fix also passes its runtime and focused production
regressions. The corrected legacy repair now also passes its runtime regression,
and the AVG proof-speed change passes the fresh SQL proof at the original limit.

| Finding | Evidence and consequence | Main-based fix branch |
|---|---|---|
| Outer joins retain uniqueness on NULL-extended keys | Native metadata regression plus an actual `TEliminateLeftJoinRule` transformation yields a bounded counterexample. With two table rows whose `g` is NULL, a full join of an empty input creates two NULL keys; eliminating a later null-equal left join changes four rows into two. The ordinary-equality control proves equivalent. This is a correctness defect in the new representation; an SQL/runtime producer path has not yet been demonstrated. | `fix/rbo-outer-join-unique-keys` (`8abc97cd8925`) |
| Lookup advertises part of a composite primary key as unique | Native regression fails when only one of two key columns is projected. The fix requires the complete key. Current evidence establishes invalid metadata, not a concrete SQL wrong result. | `fix/rbo-table-lookup-complete-key` (`caa8350a7d46`) |
| Full/exclusion joins retain one-sided shuffle metadata | Native regression confirms a distribution claim that unmatched rows need not preserve. Consumers can incorrectly omit an aggregation shuffle. A fresh SQL capture verifies with shuffle elimination disabled and produces an SMT counterexample when enabled: an empty left input and two distinct right-side keys need the omitted hash shuffle. Runtime replay confirms the wrong result: enabling shuffle elimination produced 20 separate NULL groups instead of one group with count 32. After the fix, both modes return `[[#;32u]]` and both SQL snapshot pairs verify at two rows/two tasks. | `fix/rbo-full-join-shuffle-metadata` (`779a2609e1d5`) |
| Grouping loses uniqueness when the input key is unknown | Native regression confirms that an empty input-key set suppresses the valid grouping-key guarantee. This loses optimization opportunities. | `fix/rbo-aggregate-key-uniqueness` (`ebdef4e6fcb2`) |
| A resultless aggregate used as a GroupingSets template is bypassed | Native before/after regression passes with the template-specific fix. Repeated empty grouping sets must retain their multiplicity. The fresh seeded SQL runtime regression passes for both empty and populated input. | `fix/rbo-grouping-sets-template` (`a2eb69efde5b`) |
| Filter fusion mutates the plan but reports no transformation | A native invocation changes the tree but records zero transformation events. Returning a fresh Filter restores the rule framework's invalidation, restart and capture path. The pre-fix native assertion fails as expected. Post-fix native and production tests pass, including transformation capture and parent-property recomputation. No SQL wrong result is attributed to this reporting defect. | `fix/rbo-filter-fusion-reporting` (`171c826f9ca6`) |
| Decorrelation loses the row of an empty scalar aggregate | Fresh SQL execution with new RBO enabled returns NULL for correlated `COALESCE(SUM(...), 0)` on an empty inner table, where zero is required. A fresh exact-lowered snapshot pair also yields a counterexample for an unmatched outer key. Domain-row restoration previously ran only for COUNT. The separate fix restores the row for all keyless aggregates while retaining COUNT's zero repair; After the fix, all six SUM/MIN/MAX empty/populated runtime cases pass, including all-NULL and unmatched groups. The SUM bounded proof and three focused production tests pass. COUNT and MiniQ41 proofs remain green. | `fix/rbo-empty-scalar-aggregate-domain` (`e7cffe6b9046`) |
| Legacy SQL grouping also restores the correlated scalar domain only for COUNT | With the new-RBO fix applied, the new-mode runtime cases pass but old-mode SUM still returns NULL on empty input. The legacy `BuildGroup` has the analogous COUNT-only condition. Its separate fix extends restoration to reviewed NULL-default aggregates while retaining existing COUNT behavior and leaving arbitrary factory defaults unchanged. The first candidate still failed: it read the YqlAggFactory wrapper instead of its function-name atom. The corrected candidate decodes that exact factory and adds COUNT controls. Its fresh runtime regression now passes all ten SUM/MIN/MAX/COUNT empty/populated cases in the combined workspace; an isolated build of that fix branch has not been run. | `fix/yql-empty-scalar-aggregate-domain` (`4b301d99eb09`) |

The runtime comparison uses explicit optimizer flags, disables fallback, and
forces YqlSelect translation in both modes. Both optimizer implementations have
an affected path; this is not a defect unique to enabling new RBO.

### Verifier coverage and validation

Native checks now cover logical GroupingSets independently of the expansion
rule, fixed-width COUNT(DISTINCT), exact required-member `Just` wrappers,
null-equal join keys, typed StablePickle keys including String/Utf8, all eight
integer AVG source types with both nullabilities, and native ANSI Rank. The
integer AVG count-at-most-two contract and abstract-result mismatch handling
remain enforced. Runtime text-packer checks include NULL versus empty strings,
embedded zero bytes, UTF-8, arbitrary String bytes and length-header boundaries.

The source-bound R2 gate passes all **966 Python/style/import checks**
(932 Python tests), **28 localizer checks** (26 tests), **eight SQL integration
tests**, and **four production tests**. Shared-query preparation and retention
of zero-capture SQL evidence also pass. The native run reports **77 of 79 tests
passing**: the two failures are fixture setup errors, a missing diagnostic-prefix
flag and a source-only stage incorrectly passed through a compute-stage helper.
Both fixture corrections are applied for the next run. The serial build peaked
at 15.18 GiB under the 16 GiB process-tree guard; tests peaked at 3.21 GiB.

The new-RBO and corrected legacy scalar-domain fixes pass the empty/populated
runtime cases. COUNT, MiniQ41 and SUM bounded proofs pass. Correlated AVG now
also passes the fresh SQL proof at the original 10-second limit on CPUs 12 and
13. Its bounded-count encoding retains the saved-pair measurement of 5.06
seconds versus a previous UNKNOWN at 10 seconds, with the formula growing from
59,151 to 83,471 bytes. This is a focused query result, not a corpus-wide speed
claim. R2 artifacts, source receipts and test traces are retained locally under
`/tmp/rbo-broad-audit-20261020/coverage-gates-r2-20261020`.

The recursive gate is still incomplete: legacy `cpp_ut` fixtures have not been
migrated to the current RBO API. Native and Python results do not replace that
gate. The full fresh benchmark census is recorded above.

### Construction performance

Two additional performance candidates were measured and left unapplied. On the
fresh TPCH q2 pair, keeping two rendered bodies under the same 128 MiB cap
preserved all formula bytes but took 85.73 seconds versus 83.54 seconds for the
baseline. Raising structural sharing's scope bound from 16,384 to 262,144 nodes
reduced formula bytes by about 1.1% but took 87.65 seconds. Neither improved
proof coverage. Repeated rendering of distinct obligations still consumes the
solver deadline before Z3 can attempt the semantic proof.


Retaining wide shared Boolean guards reduces repeated leaf copying. On dated,
identical Q31 snapshots, construction plus rendering CPU time drops from
42.95 to 35.11 seconds, formula bytes from 133,415,567 to 90,665,078, and peak
RSS by 16.1%. Q64 construction plus rendering CPU time drops by 5.9%.
These are construction/rendering measurements, not solver proof speedups.
All 69 focused SMT tests, including wide-guard solver equivalence checks, pass.
The earlier absent-row comparison change reduces the 23-slot checked-LIMIT
construction case from 37.45 to 0.181 seconds, with a solver equivalence check.

The later bag-count and equality-reuse changes retain 21 independent symbolic
bag-equivalence checks. On a saved wide-bag case, CPU drops from 6.52 to 0.97 s
and formula bytes from 5.92 MB to 1.21 MB. Saved TPCH q3/q6/q14 pairs verify both
before and after; solver CPU changes from 4.534/0.517/12.550 s to
2.460/0.324/7.064 s. These small saved-input measurements are not a fresh corpus
performance claim. Q64 did not improve in the controlled construction comparison.

Receipts and exact artifacts are retained under `/tmp/rbo-broad-audit-20261020/`.
`post-metadata-scalar-boundaries` contains all four latest suites (2,084 files),
source/binary hashes and the full working-tree patch. The pre-fix full-join runtime
and earlier SQL/model failures are in `corridor-and-sql-integration`.
`small-proof-performance` holds the saved-query measurements.

Large builds now use **one CPU and one build worker**, one linker, and the opt-in
low-memory links. Tests use two CPUs and one test worker. The guard keeps the
hard per-process allocation cap at 8 GiB and samples a 16 GiB process-tree RSS
cutoff; it is not a kernel cgroup limit. The latest complete build peaked at
15.14 GiB, native tests at 0.86 GiB, SQL tests at 1.94 GiB, and production tests
at 1.15 GiB. Two earlier overlapping-link attempts stopped at the guard; worker
count was reduced without raising the limit. Sources remain frozen during builds.

## Rebased corpus measurement (2026-10-19)

The SMT branch is based on fetched main `6ceb698dd99a525fc885121dfdbcad9deca11974`.
The complete formula census at `4e7f9c024ddf` and the fixed 50-query proof run
at `0e7c1886047d` give the following comparison against the rebased baseline
`2f53bf4498b0`:

| Measure | Rebased baseline | After admission and construction changes |
|---|---:|---:|
| TPCH whole-query formulas | 15/22 | 15/22 |
| TPCDS whole-query formulas | 30/99 | 48/99 |
| Total formulas | 45/121 | 63/121 |
| TPCH pinned bounded proofs | 10/16 | 10/16 |
| TPCDS pinned bounded proofs | 14/34 | 22/34 |
| Total pinned bounded proofs | 24/50 | 32/50 |

The remaining census results are one UNKNOWN, 50 UNSUPPORTED and seven
OPTIMIZER_FAILURE. The proof run has four UNKNOWN and 14 UNSUPPORTED, and
preserves every baseline proof. Its UNKNOWN cases are TPCH q15 and TPCDS
q43, q50 and q76; the construction UNKNOWN is TPCDS q17.

All 22 TPCH queries and 92/99 TPCDS queries prepare. The seven TPCDS failures
are q14, q24, q39, q44, q47, q57 and q70. The query-mode diagnostic confirms
that all seven prepare as generic queries with `_KqpEnableSpilling=true`;
both default data-query and default generic-query settings fail. The pinned
corpus configuration was retained. This diagnostic is not a wrong-result finding.

The policy gates still fail: the required 121 formulas and 50 proofs have not
been restored. Unsupported cases include GroupingSets, StablePickle,
compiled LIKE/text and Date wrappers, and additional window shapes. There
were no counterexamples in the fixed proof run. `UNKNOWN` remains unproved;
formula construction is not proof. The bounds remain two rows per table and
two tasks, with 60 seconds of solver time per query.

The eight new pinned proofs are TPCDS q15, q34, q46, q48, q68, q69, q73 and q97.
All eight re-proved in the optional nonempty-output sweep. Seven have modeled
successful nonempty executions on both boundaries; q34 is UNSAT on both at two
rows per table. Its proof therefore does not exercise nonempty output at this
bound. SAT is modeled reachability, not runtime replay or exhaustive branch
coverage. Receipts and input/tool hashes are in
`/tmp/rbo-main-audit-20261019/new-proof-nonempty-4e7f9c024dd`; peak RSS was 0.13 GiB.

The first refreshed census used a 60-second construction ceiling and emitted
60 formulas, but TPCDS q2 timed out on the same snapshots that previously
completed in about 53 seconds. The final census uses 120 seconds for construction
and rendering. q2 completed in 43.9 seconds, q4 in 80.3 seconds, and q64
in 119.2 seconds; q64 remains close to the ceiling and timing-sensitive.
Memory limits are unchanged. The proof run used the stricter
60-second construction ceiling; the subsequent change affects only the benchmark
construction allowance and its documentation, with verifier semantics and the
proof policy unchanged. The source binding is recorded in
`/tmp/rbo-main-audit-20261019/final-source-binding.json`.

Reports, input/source hashes and available verdict artifacts are archived locally:

- `/tmp/rbo-main-audit-20261019/census-4e7f9c024ddf-20261019T185443Z`
- `/tmp/rbo-main-audit-20261019/proof-floor-0e7c1886047d-20261019T184213Z`
- `/tmp/rbo-main-audit-20261019/census-0e7c1886047d-20261019T183441Z`

These are dated measurements, not replacements for the checked-in coverage policy.

## Main-source fixes and verifier validation

Five production fixes are isolated on branches whose direct parent is fetched
main `6ceb698dd99a525fc885121dfdbcad9deca11974`. Each is also applied on the SMT
branch. These defects were confirmed by focused native regressions and source
audits, separately from the bounded corpus proofs.

| Branch / commit | Defect and validation on the standalone branch |
|---|---|
| `fix/required-key-presence-ranges` / `bd7f15e85e1` | `RangeFor(Exists/NotExists)` sent required keys through the optional/Pg-only range builder. Required `Exists` now covers every value, including NaN; `NotExists` is empty. Both `TOptimizeYqlExpr::RangeForExists*` tests passed, including unchanged optional/Pg behavior. |
| `fix/string-capacity-overflow` / `5c953edbb8e` | Four string growth paths computed spare capacity with wrapping 32-bit addition. Capacity now saturates at `UINT32_MAX`; compile-time boundary checks cover the first overflowing size and the maximum. `TMiniKQLStringUtils::MakeStringWithPad` and `SubstringWithLargeOffset` passed. Multi-gigabyte allocation tests were excluded. |
| `fix/string-padding-overflow` / `6fc6fde79ee` | `MakeStringNotFilled(size, pad)` could wrap its allocation size. The regression uses `size=1, pad=UINT32_MAX`, which takes the embedded-string path on the old code without a large allocation. Both `TMiniKQLStringUtils::MakeStringWithPad*` tests passed. |
| `fix/unordered-window-rank` / `997021ffc1e` | Native `RANK` and `DENSE_RANK` without ORDER BY passed a null condition to `If`. They now keep rank 1 for all peers. Both unordered regressions failed on main while the ordered/row-number control passed; all three tests passed after the fix. Global and partitioned SQL comparisons passed on the combined SMT branch for row and column storage. |
| `fix/window-sort-duplicate-keys` / `0b92e118b26` | A column shared by PARTITION BY and ORDER BY was emitted twice into WideSort, which rejects duplicate keys. Keeping the first occurrence preserves lexicographic order. Both native regressions failed on main and passed after the fix, covering rank/dense-rank/row-number, both directions, and a trailing distinct key. SQL comparisons passed on the combined SMT branch for row and column storage. |

The current guarded runs use two CPUs and build workers, one linker, and one
test worker. The guard enforces an 8 GiB allocation limit per process, a 64 GiB
virtual-address limit, and a sampled 16 GiB aggregate RSS cutoff; it is not a
kernel cgroup memory limit. Server test executables use the documented opt-in
`RBO_LOW_MEMORY_LINK` setting. Earlier focused tests used at most four CPUs and
workers under the same memory limits. Range/capacity/padding regressions peaked
at 4.27/2.60/2.68 GiB; the isolated window regressions at 9.30/9.28 GiB.

Both `KqpRboYql::WindowFunctions` storage variants passed SQL execution on the
combined branch, including the two new window regressions. That run passed 910
checks and peaked at 15.50 GiB. Its local evidence is
`/tmp/rbo-main-audit-20261019/window-sql-and-python-ca5768ddb64`.

The latest verifier validation passed **884 Python tests and 32 native tests**,
plus lint/import checks and the static-IN real-host integration test. Two real-host
correlated COUNT tests still fail closed at unsupported `StablePickle`, after
progressing beyond the earlier empty-aggregate-traits rejection. Their expected
verdicts were not weakened. The historical COUNT test's counterexample expectation
still needs reevaluation once the current null-safe join representation is modeled;
main already contains `RestoreEmptyGroupCounts`. The old `cpp_ut` target also
remains unmigrated (281 exporter and 14 limit-rule fixtures); the full recursive
C++ gate is not green. Latest evidence and test traces are in
`/tmp/rbo-main-audit-20261019/key-only-validation`, with peak RSS 15.36 GiB.

The rebased trusted path now admits variadic UnionAll, audited String/Utf8 static
membership, and grouping-only aggregates with nonempty keys. Validation checks
NULL groups, distributed partial/final deduplication, repeated union inputs,
String/Utf8 byte semantics, and deliberately changed branches, literals and grouping
keys that must produce counterexamples. Keyless/traitless aggregates and unbound
DistinctAll aliases remain rejected.

Other restored native coverage includes initial type annotation after FromExpr,
repeated result positions, stored-String copies, whole-partition Decimal SUM/AVG,
shared logical reads, correlated capture bindings, passive Replicate ports and
exact scalar cardinality guards. Replicate tests retain a shared LIMIT choice,
reject incorrect ports, and detect changed join/hash keys. Reachable scalar-guard
failure returns UNKNOWN through a mandatory totality check.

Native Sort/Merge export was repaired to record physical NULL placement (first
in ASC, last in DESC). Its two tests failed before the repair and passed afterward,
compare with the production optional-value comparator, and detect reversed NULL
placement. This is a verifier encoding repair, not a newly attributed SQL bug.
Evidence is in `/tmp/rbo-native-null-order-before.log` and
`/tmp/rbo-native-null-order-after.log`.

A single paired TPCDS q31 profile measured rendering at **26.25 s before / 24.60 s
after**, and model construction plus rendering at **42.35 s / 41.05 s**. The
133,415,564-byte SMT formula is byte-identical, SHA-256
`b70019449669c6a308e9fc1ac955ec5246478622bc9faccb173525b631612671`.
This is about 6% rendering and 3% combined improvement on that saved input, not
a general solver-speed claim. Receipts and package/input hashes are in
`/tmp/rbo-main-audit-20261019/renderer-comparison-17b00fd9359`.
Large newly admitted constructions still need performance work; the deadline
bounds their cost and returns UNKNOWN without claiming a proof.

## Historical runtime check (2026-09-05)

On **2026-09-05**, the manual paired diagnostics at source `b000dfaba5f` observed:

- **Open: Decimal SUM partition dependence.** Identical `Decimal(35,0)` rows
  `(M, -M, M)`, where `M = 10^35 - 1`, produced `M` with one partition and
  `inf` with two, under both new RBO and legacy optimization. The
  [partition-invariance diagnostic](runtime_ut/decimal_sum_runtime_ut.cpp),
  `DecimalSumRuntimeDiagnostic::PartitionInvariantAcrossOptimizerModes`, first
  validated physical partition counts, exact input rows, and hash routing;
  it then failed with `CONFIRMED_MISMATCH`, not `HARNESS_ASSUMPTION_FAILED`.
  This is a shared aggregation/runtime issue, not an RBO-only finding.
- **Fixed: shared-IU String IN result loss.** The paired
  [String-IN diagnostic](runtime_ut/string_in_runtime_ut.cpp),
  `StringInRuntimeDiagnostic::SharedIUSemiJoinPreservesFactJoinPredicate`, passed:
  both optimizers returned `("same", 10)` with CBO disabled.

The raw run log is local `/tmp/rbo-auditability-known-bugs-runtime.log`, not a
checked-in evidence bundle. These are dated observations; rerun the focused
diagnostic before claiming that a later revision still reproduces a failure.
See [runtime assumptions and setup](runtime_ut/README.md).

The passing `DecimalSumAccumulatorOverflowRequiresHeadroom` test in
[the C++ scalar-contract tests](cpp_ut/semantic_snapshot_exporter_ut.cpp) also
demonstrates the underlying nonassociative Decimal addition. It is not a
substitute for the distributed runtime check. The verifier rejects this unsafe
overflow domain; its bounded proofs do not certify this witness.

## Fixed production optimizer defects

The eleven historically runtime-confirmed defects remain grouped by root cause
below. “Fixed” records the repair and retained regression, not a universal proof
or a claim that every listed runtime test ran in the latest verifier gate.
The production tests below live in [KqpRboYql](../../../ut/rbo/kqp_rbo_yql_ut.cpp).
The correlated-rule and stage-assignment C++ fixture files named below are
absent after the native API migration. They are historical evidence, not current
coverage; their replacements still need to be restored. The surviving older
`cpp_ut` target also requires migration.

| Historical cases | Retained focused regression / detector |
|---|---|
| 1: unrelated `NOT flag` leaked negation into a later positive `EXISTS` (`95a2afad1d3`). | `KqpRboYql::ExpressionSubquery`, the `NOT bar.flag AND EXISTS` case, checks the expected row. |
| 2–6: scalar multirow cardinality; lost subplan binding discovery; unlowered scalar projection; invalid empty optional branch; eager local cardinality error with an empty consumer (`e1e3419012c`, `52a1d7c4084`, `9e50d234264`). | `KqpRboYql::ExpressionSubquery` checks aggregate/singleton/computed projections, typed NULL, same-IU bindings, multirow errors, and empty-consumer success. [ScalarSubplanEvaluationTest](ut/test_subplans.py) separately checks model cardinality and demand. |
| 7: CBO/input order suppressed an inherited nested-scalar error when the top-level consumer was empty (`cab0dd1e89c`; model repairs `125962c87df`, `1aaf281c07a`). | `KqpRboYql::ExpressionSubquery`, `nestedMultirowScalarWithEmptyConsumer`; [limit rules](cpp_ut/limit_pushdown_rules_ut.cpp) `KeepsMarkedJoinOutsideCboWhileOptimizingBothSides` / `DoesNotConvertMarkedCrossJoinThroughFilterPushdown`; model `test_inherited_scalar_error_is_observed_without_a_consumer_input_row`. |
| 8: correlation pull-up changed empty keyless `COUNT(*)` from zero to NULL. | `KqpRboYql::CorrelatedSubquery` checks projection, Filter consumption, and the originally grouped negative case. Historical correlated-rule fixture `InlineDirectCountAddsExactOptionalZeroRepair` checked the repair. The [integration](integration_ut/optimizer_snapshot_pair_ut.cpp) `RealHostFindsCorrelatedScalarCountEmptyInputBug` retains the historical detector; its current StablePickle admission failure and old verdict expectation are recorded above. |
| 9: shared-IU equality was consumed as an invalid semi-join key, losing q56/q60 rows (`98176b0b48c`, `4f73b38aaaf`). | Paired String-IN diagnostic above; `KqpRboYql::SharedIUStringInPreservesFactJoinPredicate`; [limit rules](cpp_ut/limit_pushdown_rules_ut.cpp) `PushesSharedIUPredicateToLeftOfLeftSemiJoin`. The two workload candidates shared this one root cause. |
| 10–11: computed Map/`UNWRAP` crossed TopSort, Filter, or Join and evaluated discarded rows (`c2c66fb1d7b`, `564010e2e4e`). | [Runtime integration](integration_ut/optimizer_snapshot_pair_ut.cpp) `RealRuntimeStringUnwrapOrderLimitDemand` / `RealRuntimeStringUnwrapJoinDemand`; `KqpRboYql::PushAppendKeepsComputedExpressionAboveFilter` / `PushAppendKeepsComputedExpressionAboveJoin`; [limit rules](cpp_ut/limit_pushdown_rules_ut.cpp) `DelaysPureProjectionUntilAfterTopSort`. |

## Bounded routing findings and robustness repairs

These are separate from the eleven runtime-confirmed defects. Their archived
full-query `UNKNOWN` results are neither proofs nor evidence of current bugs.

| Finding and scope | Existing focused checks |
|---|---|
| q12: hashing the full Aggregate key split a window partition. Routing repaired in `70ab3d3631c`; historical post-fix full query was `UNKNOWN`. Physical compilation rejected `YqlAggWin`, so no runtime confirmation. | [Window SUM](ut/test_window_sum.py) `test_hashing_all_aggregate_keys_can_split_a_window_partition` detects the bad routing; `test_hashing_the_window_partition_preserves_the_logical_result` checks the repair. Historical stage fixture `AggregateThenWindowHasTwoShuffleBoundaries` checked production topology. |
| q49: global ranks became task-local under `HashV2(item)`. Repair `27e3f260017` preserves dependencies and gathers serially. The historical extracted fixed-database slice distinguished serial from hash; full query/mutation remained `UNKNOWN`, and `YqlWin` prevented runtime replay. | [Window RANK](ut/test_window_rank.py) `test_global_rank_serial_gather_proves_and_hash_split_is_wrong`; Historical stage fixtures `ExactGlobalRankGathersSerially` / `GlobalRankDependenciesPreventRatioAndTraitPruning`. These deliberately bad-plan detectors do not assert that current production still chooses bad routing. |
| Robustness: source window metadata names became inconsistent during normalization (`97a03c64ab9`), causing type-annotation failure. | [Integration](integration_ut/optimizer_snapshot_pair_ut.cpp) `RealHostCapturesExactTpcdsGlobalRanks` checks both supported semantic boundaries despite later physical rejection. This is a shared capture guard, not a dedicated replay of the original annotation failure. |
| Robustness: an untracked window entered a repeated preferred-alias rewrite (`68eb64102c7`). | Historical stage fixture `UntrackedRawRankBlocksPreferredAliasRewrite` checked rejection without replacing the expression. This guarded preparation/termination, not wrong-result behavior. |

## Model corrections are not production findings

The [historical model-candidate classification](history/pre-audit-refactor/BENCHMARK_COVERAGE.md#curated-proof-floor-and-focused-results)
must remain separate from runtime attribution:

- TPCH q6/q14 were model false positives from equivalent opaque Decimal/Boolean
  wrappers. [Exporter regressions](cpp_ut/semantic_snapshot_exporter_ut.cpp)
  `LowersExactBoolCoalesceFalseThroughIfPresent` / `FoldsExactConstantDecimalJust`
  guard the correction. The same file's `FoldsDirectTextLiteralDateSafeCastsExactly`
  and `FoldsTextLiteralDateSafeCastInActualOlapFilterDialect` guard TPC-DS q5's
  false positive: opaque Date casts admitted dates outside the real predicate.
- TPC-DS q79's opaque integral cast could return NULL for an in-range value;
  q88's opaque arithmetic disagreed with constant folding. [Scalar regressions](ut/test_scalar.py)
  `IntegralSafeCastTest` / `IntegerArithmeticTest` guard those model corrections.
  Corrected q6/q14/q88 are in the [current proof floor](benchmark_ut/coverage_policy.json).
- q77's old candidate was not rediscovered after exact Date-cast modeling, but
  the corrected full and fixed-witness checks were `UNKNOWN`: neither a current
  candidate nor a confirmed false positive. No fresh q77 replay is claimed.
- The q56/q60 witnesses led to defect 9 and were invalidated after its repair;
  subsequent archived `UNKNOWN` solver runs do not reopen that bug.
- Rank's former model treated duplicate Decimal NaNs as different peers.
  The independent `KqpRboRankContract::ForcedYqlSelectDecimalPeers` runtime
  witness establishes ANSI `AggrEquals` behavior: NaNs are peers, as are NULLs.
  The generalized Rank kernel now counts only strictly preceding rows in the
  same partition. This is a model correction, not a newly found optimizer bug;
  the earlier q49 task-routing evidence remains separately scoped above.
- **TPC-DS q40, unresolved (2026-09-05).** The new census and same-input replays
  return `COUNTEREXAMPLE` on both `77c6028e3f8` and the literal-SUM optimization;
  their canonical SMT is byte-identical. Initial `Just(SafeCast(Int32(0),
  Decimal(7,2)))` and Decimal `Coalesce` wrappers are opaque, while the final
  snapshot represents their zero/default behavior exactly. This suggests a
  model-abstraction candidate, not a new optimizer regression. Exact wrapper
  modeling and replay are the next check; no runtime confirmation is claimed.
  Paired inputs and raw receipts are local
  `/tmp/rbo-frontier-promotions-QXQWHA/q40-{baseline,current}/`.
  A fresh 2026-09-06 replay still returns the candidate in 2.131 seconds, with
  canonical SMT SHA-256 `d70f09a1d5d708bbb95647c80f29bec5fbf2def04eee4d85bbe36f89b3c849ef`.
  Its raw evidence is `/tmp/rbo-totality-final-census-47MpAn/q40/`; the
  runtime attribution remains unresolved.

## Run only the evidence you need

From the repository root, reproduce the open runtime issue without external YDB
targets (the test owns its embedded instances):

```bash
set -o pipefail
python3 ydb/core/kqp/opt/rbo/verification/tools/run_limited.py \
  ./ya make --build relwithdebinfo --threads=2 --link-threads=1 \
  --test-threads=1 --dist-store-threads=1 --memory-limit=48GB \
  -tA --test-tag ya:manual \
  ydb/core/kqp/opt/rbo/verification/runtime_ut \
  -F '*DecimalSumRuntimeDiagnostic*' 2>&1 | tail -n 100
```

Use `-F '*StringInRuntimeDiagnostic*'` for the passing paired fix regression.
For the fixed scalar group, use target `ydb/core/kqp/ut/rbo` and
`-F '*KqpRboYql::ExpressionSubquery'`; use `'*KqpRboYql::CorrelatedSubquery'`
for the COUNT cases. Other exact test names above work with their existing
`cpp_ut`, `integration_ut`, or `ut` targets; no extra runner or fixture copy is
needed. Manual runtime tests and `KqpRboYql` are outside the recursive verifier
gate. Preserve a dated result before changing a finding's status; a green
formula dashboard, finite-domain differential test, or `UNKNOWN` cannot replace
runtime confirmation. Tests at these different evidence layers are not duplicates.
