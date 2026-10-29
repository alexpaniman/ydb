# Native SQL frontend for RBO

The native path parses the shared SQL v1 grammar, binds names and types in its
own representation, and constructs new RBO operators directly. It skips YQL
SQL translation, `CompileExpr`, graph type annotation, `BuildKqlQuery`,
`RewriteSelect`, and `PlanConverter` before entering RBO.

This is an opt-in implementation of the first migration milestone, not a full
YQL replacement. Existing catalog/session infrastructure, RBO schema wrappers,
and the physical backend remain in use. After RBO, the backend may still run
YQL annotation and lowering. Removing those dependencies is a later boundary.

```
SQL -> shared grammar parser -> compact syntax
                                  |
                         catalog + native binding
                                  |
                    typed scalar arena + relations
                                  |
                       direct RBO construction
                                  |
                  existing physical compiler/runtime
```

## Routing and integration

`TTableServiceConfig.NativeFrontendMode` is compile-cache invalidating:

- `NATIVE_LEGACY` (default): preserves the existing frontend/configuration.
- `NATIVE_REQUIRED`: compile natively or report an error; never retry YQL.
- `NATIVE_AUTO`: compile natively when supported; delegate the whole query to
  the legacy YQL optimizer only for a structured `TUnsupportedFeature`.

The host supports native data, scan, and generic read-query preparation.
It loads catalog metadata asynchronously through the existing authenticated
gateway and binds against plain native column descriptions. Parameter names
are adapted to the backend's `$name` convention at the RBO boundary.
The compile service bypasses its YQL AST cache in native mode. Internal
ANALYZE requests retain their existing legacy path.

Fallback reports `[native_frontend_fallback]`, `compiler=legacy-yql`, a stable
feature identifier, source position, and reason. It does not run the old YQL
frontend into new RBO. Invalid supported syntax/types, resource-limit errors,
internal errors, and native RBO failures do not trigger fallback. Native
explain ASTs identify `native-read-v1` and include the native relational plan.

The APIs are:

- `ParseNative` / `CollectTables` / `BindNative`: separate parsing
  and binding around asynchronous metadata I/O (all in `NFrontend`).
- `CompileNative`: standalone compile/routing decision with a catalog callback.
- `ConvertNativeToRbo`: consume a bound program once at the optimizer boundary.
- `CompileNativeToRbo`: synchronous convenience API using the same grammar and
  execution profile, with no fallback.

The CLI reports fallback decisions but cannot execute the legacy compiler.

## Grammar and parser migration

`grammar/default` and `grammar/ansi` generate independent, namespaced C++
parsers from `yql/essentials/sql/v1/SQLv1Antlr4.g.in` at build time. They depend
on the generic ANTLR C++ runtime, not YQL parser wrappers, protobuf parse trees,
translation, or type libraries. Strict recognition covers the full shared
grammar; the translator explicitly rejects productions outside the native
semantic subset. Error-recovered trees are never compiled.

The generated parser is the default. The handwritten parser remains available
for measurements and gradual migration. `--parser verify` compares complete
syntax fingerprints from both parsers, including precedence, clauses, names,
and types, before using the handwritten result. This is a test/development
mode; it intentionally pays for both parsers. Standalone differential tests
cover representative supported constructs and both lexer modes.

ANSI mode changes string escaping and nested-comment behavior. Double quotes
remain string quotes in this YQL grammar; backticks quote identifiers. Leading
`--!syntax_v1`, `--!antlr4_parser`, and `--!ansi_lexer` directives are handled by
the native compiler. Text resembling a directive inside a string is ordinary
string data.

## Execution profile

`compiler/compiler.cpp` enforces `native-read-v1`; `--support` prints its
machine-readable description. Grammar recognition is deliberately broader
than execution support. The profile is experimental and grows with legacy
result/type parity tests.

Supported relational forms include SELECT and FROM-first SELECT, projection,
filters, aliases/stars, ordinary table reads, CTEs/derived tables, scalar/query
bindings, simple scalar lambdas, ON joins (inner/cross/outer/semi/only), grouping,
HAVING, DISTINCT, UNION ALL, ordering, limits, and multiple result statements.
Aggregates are COUNT, SUM, AVG, MIN, MAX, and SOME. A DISTINCT aggregate must be
the only aggregate in that aggregation. Computed GROUP BY keys require explicit
aliases (`GROUP BY expression AS key`, then `SELECT key`). Scan queries require one result.

Scalar types are Bool, signed/unsigned integers, Float/Double, String/Utf8,
Date/Datetime/Timestamp/Interval, Null, and one optional layer. Arithmetic,
comparisons, Boolean logic, CASE/IF, COALESCE, IN lists, BETWEEN, null tests,
byte-string concatenation, Length, StartsWith/EndsWith, ASCII case functions,
and simple constant LIKE patterns have native operations. Primitive data
constructors require string literals and produce required values. Explicit
casts in this profile currently target integer types; runtime inputs to those
casts must be integral. Widening integral casts preserve non-nullability.
CASE requires an explicit ELSE branch. Supported pragmas are TablePathPrefix, YqlSelect="force", and
AnsiImplicitCrossJoin.

Explicitly outside the profile:

- DML/DDL, USE, scripting effects, arbitrary pragmas/settings, views, temporary
  and external tables, table hints, sampling, and PostgreSQL syntax/types.
- Scalar/IN/EXISTS subqueries, correlation, JOIN USING, windows, grouping sets,
  UNION DISTINCT/INTERSECT/EXCEPT, ordering across UNION ALL, and stars over joins
  (which require YQL's column-name coalescing rules).
- UDFs, higher-order values, containers/structs/tuples, nested optionals, Decimal.
- Mixed signed/unsigned or mixed Float promotion, floating division, substring,
  bare-NULL arithmetic, dynamic text parsing, and nonintegral runtime casts.
- General LIKE patterns and unverified function aliases. Invalid aliases are
  delegated in auto mode so legacy YQL supplies its own diagnostic.
- System columns, typed-string suffixes, and string escapes beyond newline,
  carriage return, tab, zero, quote/backtick, and backslash.

These exclusions avoid known semantic differences in the early expression
prototype. The lower-level arena and reference evaluator can represent more
operations than this profile permits; accepting an operation there does not
certify SQL compatibility.

## Expression model and performance

The scalar arena contains only typed execution operations: no unresolved names,
SQL subqueries, lambda applications, provider macros, or type expressions.
Children precede parents. Constructors resolve each node's type and insert
explicit conversions from already typed children. Binding does not run a type
fixpoint or repeat semantic lowering passes. Named-query/lambda expansion can
duplicate work and is bounded, as are input bytes, tokens, nodes, and depth.

Nodes are immutable after construction in a query-owned **append-only** arena.
Existing IDs remain valid when the arena grows. Same-arena composition appends
only the new operation; rewrites share unchanged subgraphs and copy changed
ancestors. There is no whole-arena copy per Boolean composition. Vector element
references may be invalidated by appends; mutation is serialized per query.
Foreign-arena imports copy only reachable nodes.

Native expressions remain native through logical RBO typing and rewrites.
Physical builders request a terminal scalar graph from `LowerNativeLambda`.
Each opcode has a direct execution lowering; there is no SQL semantic rewrite
loop in that adapter. Reference evaluation and direct MiniKQL tests check this
boundary separately from host-level comparison against legacy YQL.

**Plan-quality limitation:** legacy scalar peepholes, range extraction, and
OLAP predicate/projection pushdown are currently skipped for native scalars.
Native equivalents and plan/execution parity are required before broad rollout.
A faster frontend alone does not prove faster complete query execution.

## Build and test

```
./ya make --build relwithdebinfo -tA ydb/core/kqp/opt/rbo/frontend
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/rbo -F 'KqpNative*'
./ya make --build relwithdebinfo ydb/core/kqp/opt/rbo/frontend/cli

echo 'SELECT id, value + 1 AS next FROM t WHERE value > 0 LIMIT 10' | \
  ydb/core/kqp/opt/rbo/frontend/cli/kqp_native_frontend \
  --table t 'id:Uint64,value:Int32?'

echo 'SELECT id, SUM(value) AS total FROM t GROUP BY id' | \
  ydb/core/kqp/opt/rbo/frontend/cli/kqp_native_frontend \
  --table t 'id:Uint64,value:Int32?' --parser grammar --bench 10000
```

The CLI benchmark includes parsing, binding, profile validation, allocation,
and destruction with an in-memory catalog. It excludes catalog I/O, RBO, and
backend compilation. Use identical SQL/schema/settings for parser comparisons;
do not interpret grammar acceptance or parse/bind success as execution coverage.

`python3 ydb/core/kqp/opt/rbo/frontend/tools/probe_corpus.py --output /tmp/native-coverage.json`
reports per-query native/legacy/error routing against the checked-in benchmark
schemas. The initial conservative profile accepted 36/43 ClickBench queries
and explicitly delegated all 22 TPC-H and 99 TPC-DS queries, primarily because
Decimal/casts are excluded. These numbers measure frontend acceptance only;
they supersede the broader prototype's unverified parse/bind counts.

## Initial validation and measurements

The selected relwithdebinfo checks passed 135 tests: 48 standalone frontend
tests, 15 native/RBO/host tests, and 72 shared RBO regression tests. Host tests
compare values and column names/types against the legacy optimizer, including
prepared parameters, table reads, joins, aggregation, DISTINCT, UNION ALL,
ordering, limits, generic multiple results, scans, and explicit fallback.
This is a regression suite, not proof of complete YQL compatibility.

Run `python3 ydb/core/kqp/opt/rbo/frontend/tools/bench_parsers.py --output /tmp/native-parser-bench.json`
with an idle machine to reproduce the parser comparison. A local relwithdebinfo
run on 2026-10-30 measured the following medians of five trials of 1,000 warm
compilations each (microseconds per compilation):

| Query | Shared grammar | Handwritten | Ratio |
| --- | ---: | ---: | ---: |
| Projection/filter | 47.21 | 6.98 | 6.77 |
| Grouping/sort | 139.52 | 14.40 | 9.69 |
| Join | 123.84 | 16.44 | 7.53 |
| 100 computed columns | 1859.79 | 295.64 | 6.29 |

These compare two native frontends, not complete native versus legacy YQL
compilation. The measured gap supports migrating the handwritten parser
gradually, with correspondence checks. `probe_corpus.py --parser verify`
agreed on all 36 currently accepted corpus queries, but reported 18 structural
or acceptance discrepancies in TPC-DS queries outside the execution profile
(including subqueries and trailing commas). The handwritten parser remains
experimental and is not selectable in the host; generated grammar remains
the host's parser until those boundaries are tested adequately.
