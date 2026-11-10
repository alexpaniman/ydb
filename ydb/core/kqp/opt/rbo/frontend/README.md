# Native SQL frontend and RBO expressions

New RBO has one scalar representation: typed native expressions in a query-owned
arena. SQL translation, YQL scalar graphs, `CompileExpr`, and YQL graph type
annotation do not participate in native binding or logical optimization. The
existing catalog/session services, physical compiler, and runtime remain in use.

```text
SQL
  -> fast parser / independently generated shared-grammar parser
  -> compact syntax + asynchronous catalog resolution
  -> native name binding, types, scalar DAG and relations
  -> checked TCompiledProgram
  -> owning TNativeQuery: RBO roots, ordered outputs, parameters, execution metadata
  -> existing RBO rules, CBO and stage assignment using native expressions
  -> explicit physical emission: backend types, lambdas, tables and runtime operators
  -> existing physical compiler and execution
```

## Boundaries and ownership

`ParseNative`, `CollectTables`, and `BindNative` separate syntax from asynchronous
catalog I/O. `CompileNative` combines them for callers with a synchronous catalog.
Only checked compilation can construct `TCompiledProgram`; conversion consumes it
once. A raw binder result cannot enter RBO.

`ConvertNativeToRbo` constructs ordinary RBO operators directly. Its result,
`TNativeQuery`, owns the roots, native parameter schemas, ordered output names,
and `TQueryExecutionMetadata`. Native queries do not build synthetic
`KiDataQueryBlock` graphs. The legacy compiler imports the same execution metadata
from its own query blocks.

`TExpression` holds an arena and a node ID. There is no YQL expression constructor,
legacy lambda arm, implicit export, or mutable backend annotation. Rewrites append
native nodes, share existing subgraphs, and use explicit column-ID substitutions.
Scalar factories take native types and plan properties, with no backend expression
context argument.
Copying a plan rebinds expressions, captures, read predicates and OLAP projections.

`TNativeSchema` describes rows and aggregate states. `ComputeNativeTypes` checks
operators and rebinds scalars using native types. AVG/statistical states use
arena-owned tuple shapes; nested optional types preserve each optional layer.
`TRboTable` holds catalog identity and native column types as values. Read ranges,
lookup point prefixes, OLAP predicates, sink settings and stream lookup key types remain native
through stage assignment.

Physical builders cross the boundary through `ExportNativeType`,
`ExportOperatorType`, `ExportIUType`, `LowerTable`, and `LowerExpressionLambda`.
Each export produces fresh objects in the backend context. Scalar lowering emits
fully typed terminal operations; direct MiniKQL tests require neither annotation
nor a semantic rewrite pass. Physical relational builders retain their existing
backend annotation and peephole pipeline.

The migration preserves the existing rule schedule, join enumeration, cost
formulas, stage assignment, shuffle policy and runtime algorithms. Expression
construction and inspection use the native arena. Computed ON keys are exposed
as ordinary maps and join keys before outer-join NULL padding. Native selectivity
retains the existing estimator's policies, including its capped OR heuristic and
fallback estimates, rather than changing cost decisions as part of migration.

## Admission and fallback

`EnableNewRBO` selects the native frontend. `NativeFrontendMode` additionally
controls admission:

| Mode | Behavior |
| --- | --- |
| `NATIVE_LEGACY` (default) | Legacy entry when new RBO is disabled; native automatic admission when enabled. |
| `NATIVE_AUTO` | Opt into native admission, including when `EnableNewRBO` is initially disabled. |
| `NATIVE_REQUIRED` | Compile natively or report an error. |

Automatic admission delegates **the whole query to the legacy optimizer** only
for a structured `TUnsupportedFeature`. It reports `[native_frontend_fallback]`,
`compiler=legacy-yql`, feature, position and reason. Invalid supported syntax,
type errors, resource limits, internal errors and RBO failures do not silently
retry. The old `EnableFallbackToYqlOptimizer` flag does not enable error retries;
use `NATIVE_REQUIRED` to forbid unsupported-query fallback. There is no
YQL-to-new-RBO import pipeline.

Native data, scan and generic read preparation bypass the YQL AST cache. Internal
ANALYZE requests and internal metadata maintenance retain their legacy entry. Native explain ASTs contain
`native frontend: native-read-v1`, parser selection and the native relational
plan. The CLI reports admission decisions but cannot execute the legacy compiler.

## Parsing

The default `fast` route uses a handwritten subset and invokes independently
generated ANTLR C++ parsing for unhandled syntax or syntax errors. The generated
parser shares `SQLv1Antlr4.g.in`, not YQL parser wrappers, protobuf trees,
translation or type libraries. Error-recovered trees are never compiled.

`--parser grammar` forces generated parsing; `--parser handwritten` disables
parser dispatch. `--parser verify` compares complete syntax fingerprints, including
precedence, clauses and types. It is a development check and may reject syntax
outside the handwritten subset. Binding and resource-limit errors do not retry
parsing. Correspondence tests cover the 164-query corpus, keyword contexts,
operator combinations, malformed mutations and lexer modes.

Backticks quote identifiers. Double quotes remain strings under this grammar.
ANSI lexer mode changes escaping and nested comments. Leading syntax/parser/ANSI
directives are recognized; directive text inside strings has no effect.

## Execution profile

`frontend.cpp` checks `native-read-v1`; `--support` returns the machine-readable
manifest. Grammar recognition is broader than execution support. Arena support
alone does not establish SQL compatibility.

Supported relational forms include SELECT/FROM-first SELECT, ordinary tables and supported system views,
projections and unique-name stars, CTEs/query bindings, simple scalar lambdas,
ON joins, grouping/HAVING, GROUPING SETS/ROLLUP/CUBE, windows, DISTINCT,
UNION/INTERSECT/EXCEPT including ALL, sorting/limits and multiple results.
Scalar/EXISTS/single-column IN subqueries support correlation within the checked
backend profile. Scalar subqueries preserve NULL for no row and fail for more
than one row. Scan queries require one result.

Named sources outside correlated contexts retain one producer with distinct
bindings at each use. Existing RBO replication, copying and spilling rules decide
how to execute it. OLAP predicates stay native through optimization; physical
emission uses storage operations or the existing optional scalar-apply kernel.
Predicates containing checked expressions remain intact when storage filtering
could suppress their errors.

Aggregates include COUNT, SUM, AVG, MIN, MAX, SOME and sample standard deviation.
DISTINCT groups original values before accumulator conversion. Native partial
states preserve Decimal AVG and nullable interval SUM. Window support includes
inline and named WINDOW definitions, aggregates/ranking and ROWS/RANGE frames with
checked literal offsets. Inherited windows are excluded.

Scalar types include Boolean, signed/unsigned integers, Float/Double, String/Utf8,
Date/Datetime/Timestamp/Interval, Decimal, Json/JsonDocument and NULL. Explicit
nested optional casts, Just and Unwrap preserve outer absence versus inner NULL;
general arithmetic and comparison on nested optionals are not admitted.
String `s`/`u` suffixes are supported. Integer promotion, checked casts, decimal
rounding/exponents, Unicode case conversion, selected calendar operations,
JSON_VALUE and constant LIKE patterns have runtime differential tests.

`AS_TABLE` accepts declared or supplied `List<Struct<scalar fields>>` parameters,
including nullable fields and empty structs. Source field names survive aliases,
pruning and copying. Separately, scalar List parameters and fixed tuple/list
constructors support IN membership. Dynamic list membership uses checked key
conversion and explicit ANSI/legacy NULL semantics; it emits typed dictionary
operations rather than a `SqlIn` macro. Collections cannot be relational columns.
Fixed tuple comparisons expand into native scalar comparisons with SQL NULL
semantics and lexicographic ordering.

Set operands align by name and infer common types across the chain. Missing
fields become NULL. Set ALL operations preserve duplicate counts. Output order
follows the existing common-prefix contract; comparisons with the legacy
frontend account for its different star-column order by name.

Important exclusions remain explicit:

- Writes/DDL, effects, USE, PostgreSQL, external/temporary tables, views, table
  hints/sampling and unrecognized settings.
- JOIN USING, tuple IN subqueries, duplicate star names requiring coalescing,
  and FULL JOIN residual predicates other than column equality keys.
- Subqueries in ON, GROUP BY, direct ORDER BY, LIMIT/OFFSET, scalar bindings or
  lambda bodies; ungrouped outer references after aggregation.
- Correlated windows and correlated INTERSECT ALL/EXCEPT ALL; inherited,
  DISTINCT/filtered windows, NULL treatment and unsupported frame forms.
- Arbitrary UDFs, volatile functions, higher-order values and general containers.
  Dynamic collection dictionary keys have a narrower checked scalar profile.
- Bare-NULL arithmetic, unsupported mixed Decimal operations, floating Decimal
  casts, Float-to-32/64-bit-integer casts and unverified temporal conversions.
- Dynamic LIKE patterns, `_`/escape forms, ILIKE, and unsupported JSON paths or
  handlers. Calendar intermediates cannot cross relational boundaries.

Known backend limitations are admission checks, not a second scalar interpreter.
Unsupported queries retain the legacy optimizer's behavior through whole-query
fallback. See the manifest and negative tests for precise restrictions.

## Validation and measurement

Validation is split by boundary:

- Standalone parser, binder, type, expression evaluator and corpus tests.
- Native RBO invariant tests for column IDs, sharing/captures, copying, schemas,
  and unchanged expression/type allocation counters during logical optimization.
- Direct MiniKQL scalar execution, including dynamic parameters, NULL, overflow,
  Decimal and collection semantics, without scalar annotation or compatibility
  rewriting.
- Host tests for complete compilation, populated results, ordered names/types,
  fallback, prepared parameters, caches, row/column storage and physical options.
- Original SQL regression tests admitted in native-required mode, retaining their
  SQL and result expectations.

The selectivity oracle compares 7,832 predicate roots under three statistics
configurations (23,496 comparisons). A separate 20,000-node shared Boolean DAG
checks bounded traversal without expanding the graph into a tree.

All 22 TPC-H and 99 TPC-DS benchmark queries have native-required full-compilation
checks with fallback disabled and the existing CBO invariants. Execution fixtures
cover all 121 queries. All 22 TPC-H and 54 TPC-DS fixtures have populated result
checks; the remaining 45 TPC-DS queries execute and compare every result, but
some produce empty or NULL-only results and need stronger data before claiming
populated coverage. Q4 retains the original contradictory predicates.

`KqpNativeHost::NativeCompilationBenchmark` measures complete compiler CPU and
wall time on eight queries with AST/compilation caches disabled, one warm-up and
rotating order. Current modes are legacy, native automatic and native required.
Historical YQL+new-RBO comparisons require the preserved pre-removal binary;
current admission modes are not separate compiler architectures. Request wall
time also includes execution on empty tables and is reported separately.
Parser-only CLI timings exclude catalog I/O, RBO and backend compilation.

### Completion checks, 2026-11-10

The relwithdebinfo build, standalone CLI smoke check and these suites pass:

| Scope | Passing test cases |
| --- | ---: |
| Standalone parser, binder, compiler and scalar arena | 119 |
| Native RBO, copying, decorrelation, IDs, lookup joins and OLAP | 220 |
| Existing SQL read regressions | 226 |
| Host compilation, execution, routing, parameters and caches | 68 |

The full read and host runs cover the final semantic changes, including tuple
comparisons, floating remainder and storage filtering. A subsequent API-only
cleanup removed unused backend-context arguments from scalar factories. All
targets were rebuilt and the 220 RBO/OLAP tests passed again; repeated runs are
not added to the counts above. The final benchmark uses that rebuilt binary.
The TPC execution coverage and populated-data limitations are described above.

### Complete compiler measurements, 2026-11-10

Two before/after batches compared the preserved YQL+new-RBO binary from
2026-11-09 with the final native-required binary. Each binary ran eight queries
in three modes for nine measured rounds, after warm-up: 216 samples per binary
per batch, 864 samples overall. The ratios below compare per-query median
compiler counters and use a geometric mean across queries. Cache hits and native
fallback were forbidden; every nontrivial CBO tree had to be optimized.

The host reports AMD EPYC 9654 and 128 CPUs. The first batch used normal
scheduling. An independent worktree build introduced timing variation, so a
second batch gave both binaries the same CPU affinity, CPUs 24–31. The machine
was shared in both batches. Legacy-compiler CPU counters provide a control;
their ratio is baseline divided by final, just like the speedup columns.

| Batch | Compiler CPU speedup | Compiler wall-time speedup | Legacy CPU control ratio |
| --- | ---: | ---: | ---: |
| Normal scheduling | 1.344x | 1.362x | 0.996 |
| Fixed CPU affinity | 1.503x | 1.522x | 1.027 |

Per-query CPU results, retaining both batches:

| Query | Normal-scheduling speedup | Fixed-affinity YQL+RBO, ms | Fixed-affinity native, ms | Fixed-affinity speedup |
| --- | ---: | ---: | ---: | ---: |
| TPC-H Q1 | 1.64x | 81.2 | 46.6 | 1.74x |
| TPC-H Q5 | 1.28x | 154.5 | 104.9 | 1.47x |
| TPC-H Q9 | 1.41x | 154.5 | 123.7 | 1.25x |
| TPC-H Q21 | 1.19x | 202.4 | 139.3 | 1.45x |
| TPC-DS Q17 | 1.42x | 318.6 | 185.3 | 1.72x |
| TPC-DS Q28 | 0.95x | 403.6 | 334.0 | 1.21x |
| TPC-DS Q51 | 1.62x | 171.5 | 96.6 | 1.78x |
| TPC-DS Q52 | 1.36x | 97.1 | 64.2 | 1.51x |

The measured aggregate reduction in compilation CPU is 25.6–33.5%. Q28's initial
4.9% slowdown did not reproduce with fixed affinity, where its legacy control
ratio was 1.001. Per-query gains depend on workload and scheduling; these shared
host measurements support an aggregate improvement on this workload. Compiler
wall counters exclude the empty-table execution included in request timings.

## Build and run

Use the project compiler balancer and normal relwithdebinfo build:

```bash
~/projects/clang-balancer/balance ./ya make --build relwithdebinfo \
  ydb/core/kqp/opt/rbo/frontend/ut ydb/core/kqp/ut/rbo

~/projects/clang-balancer/balance ./ya make --build relwithdebinfo -tA \
  ydb/core/kqp/opt/rbo/frontend/ut 2>&1 | tail

~/projects/clang-balancer/balance ./ya make --build relwithdebinfo ydb/core/kqp/opt/rbo/frontend/cli
printf '%s\n' 'SELECT id, value + 1 FROM t WHERE value > 0' | \
  ydb/core/kqp/opt/rbo/frontend/cli/kqp_native_frontend \
  --table t 'id:Uint64,value:Int32?'
```

Do not edit sources during compilation. Full host suites may exceed the default
shard timeout; direct binary selectors use `+Suite` or `+Suite::Test`. The
compilation benchmark accepts `--test-param NATIVE_COMPILATION_BENCH_ITERATIONS=9`
without `--fork-tests`, whose child invocation does not forward test parameters.
`tools/probe_corpus.py` reports per-query parser/binder routing; it is not an
execution test. `tools/bench_parsers.py` compares native parser routes on an idle
machine.
