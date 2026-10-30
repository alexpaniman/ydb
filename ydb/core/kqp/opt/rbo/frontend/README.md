# Native SQL frontend for RBO

The native path uses a handwritten SQL subset with a shared-grammar parser for
unhandled syntax, binds names and types in its own representation, and constructs
new RBO operators directly. It skips YQL SQL translation, `CompileExpr`, graph type annotation, `BuildKqlQuery`,
`RewriteSelect`, and `PlanConverter` before entering RBO.

This is an opt-in read-query frontend, not a full YQL replacement. Existing
catalog/session infrastructure, RBO schema wrappers, and the physical backend
remain in use. After RBO, the backend may still run
YQL annotation and lowering. Removing those dependencies is a later boundary.

```
SQL -> fast parser (grammar for unhandled syntax) -> compact syntax
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

The host supports native data, scan, and generic read-query preparation. Native
RBO roots are constructed before initializing backend providers. This path does
not construct either YQL frontend pipeline or the legacy optimizer pipeline; the
runner creates legacy optimizer pipelines only when its legacy entry is used.
It loads catalog metadata asynchronously through the existing authenticated
gateway and binds against plain native column descriptions. Parameter names
are adapted to the backend's `$name` convention at the RBO boundary.
The compile service bypasses its YQL AST cache in native mode. Internal
ANALYZE requests retain their existing legacy path.

Fallback reports `[native_frontend_fallback]`, `compiler=legacy-yql`, a stable
feature identifier, source position, and reason. It does not run the old YQL
frontend into new RBO. Invalid supported syntax/types, resource-limit errors,
internal errors, and native RBO failures do not trigger fallback. Native
explain ASTs identify `native-read-v1`, the selected parser (`handwritten` or
`grammar`), and the native relational plan.

The APIs are:

- `ParseNative` / `CollectTables` / `BindNative`: separate parsing
  and binding around asynchronous metadata I/O (all in `NFrontend`).
- `CompileNative`: standalone compile/routing decision with a catalog callback.
- `ConvertNativeToRbo`: consume a validated `TCompiledProgram` once at the optimizer boundary.
  Only checked compilation can construct this type; raw binder output cannot
  be passed to the converter.
- `CompileNativeToRbo`: synchronous convenience API using the same grammar and
  execution profile, with no fallback.

The CLI reports fallback decisions but cannot execute the legacy compiler.

## Grammar and parser migration

The `grammar` build target generates independent, namespaced C++
parsers from `yql/essentials/sql/v1/SQLv1Antlr4.g.in` at build time. They depend
on the generic ANTLR C++ runtime, not YQL parser wrappers, protobuf parse trees,
translation, or type libraries. Strict recognition covers the full shared
grammar; the translator explicitly rejects productions outside the native
semantic subset. Error-recovered trees are never compiled.

`--parser fast` is the default in both the CLI and host. It uses the handwritten
subset, and invokes the independent generated parser for unhandled syntax or
parse errors. The generated parser then supplies authoritative diagnostics or
an equivalent native syntax tree. This parser dispatch never invokes YQL
translation or the legacy optimizer. Binding errors do not retry parsing, and
resource-limit errors never retry either parser.

The handwritten lexer's identifier categories are generated from the original
grammar at build time. Ambiguous aliases, non-ASCII input, multiline strings,
and other uncovered syntax can use generated parsing. `--parser grammar` forces
the generated parser; `--parser handwritten` runs only the small parser.
`--parser verify` compares their complete syntax fingerprints, including
precedence, clauses, names, and types. Verify is a development tool and can
reject syntax outside the handwritten subset even if fast mode supports it.

Correspondence tests cover all 164 checked-in benchmark queries, every keyword
in identifier/alias/join contexts, operator combinations, syntax mutations, and
both lexer modes. They check rejection as well as structure. Generated parsing
is still an essential part of the fast mode's compatibility contract.

ANSI mode changes string escaping and nested-comment behavior. Double quotes
remain string quotes in this YQL grammar; backticks quote identifiers. Leading
`--!syntax_v1`, `--!antlr4_parser`, and `--!ansi_lexer` directives are handled by
the native compiler. Text resembling a directive inside a string is ordinary
string data.

## Execution profile

`frontend.cpp` enforces `native-read-v1`; `--support` prints its
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
casts cover integers, Bool, Float/Double, and String/Utf8 (text parsing still
requires literals). Total casts preserve non-nullability, including integer to
floating point. Floating division preserves IEEE infinity/NaN. Substring slices
String bytes; NULL offsets mean zero/to-end. Dynamic offsets accept Uint8/16/32;
other integral offsets must be literals that fit Uint32.
Numeric text conversion uses the generic `double-conversion` library with the
runtime's lexical policy, including leading spaces, hexadecimal integers,
and overflow/underflow to infinity/zero; it imports no YQL conversion code.
CASE requires an explicit ELSE branch. Supported pragmas are TablePathPrefix, YqlSelect="force", and
AnsiImplicitCrossJoin.

Explicitly outside the profile:

- DML/DDL, USE, scripting effects, arbitrary pragmas/settings, views, temporary
  and external tables, table hints, sampling, and PostgreSQL syntax/types.
- Scalar/IN/EXISTS subqueries, correlation, JOIN USING, windows, grouping sets,
  UNION DISTINCT/INTERSECT/EXCEPT, ordering across UNION ALL, and stars over joins
  (which require YQL's column-name coalescing rules).
- UDFs, higher-order values, containers/structs/tuples, nested optionals, Decimal.
- Mixed signed/unsigned or mixed Float promotion, bare-NULL arithmetic, dynamic
  text parsing, and runtime casts from temporal/Decimal inputs.
- Float to Int32/Uint32/Int64/Uint64: the existing runtime rounds the upper
  integer bound to Float before checking, admitting an out-of-range value.
  These casts remain explicit unsupported features; no existing runtime fix is
  included. Double to integer and Float to 8/16-bit integer casts are supported.
- General LIKE patterns and unverified function aliases. Invalid aliases are
  delegated in auto mode so legacy YQL supplies its own diagnostic.
- String literals used as identifiers (use backticks), system columns,
  typed-string suffixes, and string escapes beyond newline,
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

## Validation and measurements

The selected relwithdebinfo checks passed 140 tests: 52 standalone frontend
tests, 16 native/RBO/host tests, and 72 shared RBO regression tests. Host tests
compare values and column names/types against the legacy optimizer, including
prepared parameters, table reads, joins, aggregation, DISTINCT, UNION ALL,
ordering, limits, generic multiple results, scans, and explicit fallback.
They also check handwritten/generated parser selection through explain output
and execute both routes. This is a regression suite, not proof of complete YQL
compatibility.

The scalar extension subsequently passed 72 native tests, including direct
MiniKQL execution, numeric-text comparisons with the runtime, large integer
to Float rounding, and legacy-optimizer result/type parity with runtime
parameters. It enables floating division, scalar casts and Substring without
enabling Decimal or the remaining relational features.

Fast and generated modes have identical routing on all 164 benchmark queries:
36 native and 128 explicit legacy delegations, with no hard errors. The standalone
CLI contains no YQL or MiniKQL symbols; the frontend's only parser runtime
dependency is generic ANTLR.

Run `python3 ydb/core/kqp/opt/rbo/frontend/tools/bench_parsers.py --output /tmp/native-parser-bench.json`
with an idle machine to compare generated parsing with fast mode. A local
relwithdebinfo run on 2026-10-30 measured these medians of five trials of 1,000
warm compilations each (microseconds per compilation):

| Query | Shared grammar | Fast | Ratio |
| --- | ---: | ---: | ---: |
| Projection/filter | 48.52 | 6.18 | 7.86 |
| Grouping/sort | 130.16 | 11.64 | 11.18 |
| Join | 124.83 | 13.38 | 9.33 |
| 100 computed columns | 1832.16 | 232.12 | 7.89 |

These compare two native frontends, not complete native versus legacy YQL
compilation. Fast mode still delegates unhandled syntax to generated parsing;
its correspondence tests exercise that full dispatch path. The benchmark cases
also verify direct handwritten/generated syntax agreement before timing.

## Migration scope and baseline

The compiler is contained in four implementation files in this directory:
`expression.cpp` (types and execution operations), `parser.cpp` (fast syntax),
`grammar.cpp` (shared grammar adaptation), and `frontend.cpp` (binding and
checked compilation). `grammar/` contains build rules only. RBO conversion and
backend lowering are in the enclosing RBO directory. Tests share one `ut`
target. New language support belongs in these files rather than a new framework
or a directory per feature.

The unchanged baseline at `f9967857753` passed all 357 RBO tests on 2026-10-30.
Existing benchmark expectations are 22/22 TPC-H and 97/99 TPC-DS explain
successes; TPC-DS 17 and 44 are expected failures, not native successes. The
migration target is all 121 queries with native-required compilation and
execution validation, plus previously working YQL read-query tests. Writes
and PostgreSQL are tracked separately. Pre-existing RBO/backend defects remain
outside this patch; failures caused by native translation or migration are fixed.
