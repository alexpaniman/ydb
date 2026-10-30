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
HAVING, inline windows, DISTINCT, UNION/INTERSECT/EXCEPT (including ALL), ordering, limits, scalar/EXISTS/IN subqueries with correlation,
and multiple result statements.

Set operands align by field name, infer common types once across the chain, and
fill missing fields with NULL. INTERSECT has higher precedence; UNION and EXCEPT
associate left. A flat postfix syntax keeps long chains shallow, and consecutive
UNION ALL operands use one n-ary RBO union. DISTINCT uses ordinary grouping;
INTERSECT/EXCEPT use semi/anti joins with stable keys for equal-NULL semantics.
Their ALL variants number equal rows on each side and match those ordinals, so
duplicate counts require no row-expansion operation. Global ordering/limits and
parenthesized operand-local clauses are supported. Result field order follows
YqlSelect's common-prefix rule; the older translator emits lexical order in some
mixed-schema cases. Tests pin that existing difference explicitly.
Aggregates are COUNT, SUM, AVG, MIN, MAX, and SOME. A DISTINCT aggregate must be
the only aggregate in that aggregation. Computed GROUP BY keys require explicit
aliases (`GROUP BY expression AS key`, then `SELECT key`). Scan queries require one result.

Scalar types are Bool, signed/unsigned integers, Float/Double, String/Utf8,
Date/Datetime/Timestamp/Interval, Decimal, Null, and one optional layer. Arithmetic,
comparisons, Boolean logic, CASE/IF, COALESCE, IN lists, BETWEEN, null tests,
byte-string concatenation, Length, StartsWith/EndsWith, ASCII case functions,
and constant LIKE patterns containing literal text and % wildcards have native operations. Primitive data
constructors require string literals and produce required values. Explicit
casts cover integers, Bool, Float/Double, Decimal, String/Utf8, and narrow temporal
types (text parsing requires literals except for Date). Temporal inputs may be cast to other
temporal types. Total casts preserve non-nullability, including integer to
floating point. Floating division preserves IEEE infinity/NaN. Substring slices
String bytes; NULL offsets mean zero/to-end. Dynamic offsets accept Uint8/16/32;
other integral offsets must be literals that fit Uint32.
Integer comparisons preserve each operand's width and signedness and compare
their mathematical values, including Int64 against Uint64. They return nullable
Bool when either input is nullable; no common-type casts are needed. Other
implicit signed/unsigned promotions remain outside the native profile.
Numeric text conversion uses the generic `double-conversion` library with the
runtime's lexical policy, including leading spaces, hexadecimal integers,
and overflow/underflow to infinity/zero; it imports no YQL conversion code.
Decimal arithmetic preserves YQL infinity/NaN and rounds scale reductions to
nearest with ties to even. Precision overflow produces infinity; Decimal
arithmetic does not add a nullable layer. Constructors accept canonical literal
digits/dot or inf/nan, with literal precision/scale. Decimal-to-integer conversion
rounds through the source's integral precision before checking the integer range,
matching the existing runtime. Backend lowering emits direct typed arithmetic
and casts, without finite-only guards or extra rounding wrappers. Comparisons,
IF/CASE branches and UNION ALL use an exact common Decimal type with integers
or other Decimals. Integer widths follow SQL metadata (including 20 digits for
both 64-bit integer types), without conversion through floating point.
Decimal multiplication also accepts integer factors in either order, preserving
the Decimal operand's precision and scale. The factor remains integral: a large
integer cannot overflow an intermediate Decimal cast and change a zero product
into NaN. Overflow, infinity, NaN and NULL follow the existing runtime directly.
CASE requires an explicit ELSE branch. Supported pragmas are TablePathPrefix, YqlSelect="force", and
AnsiImplicitCrossJoin, AnsiInForEmptyOrNullableItemsCollections, and
DisableAnsiInForEmptyOrNullableItemsCollections, AnsiCurrentRow and DisableAnsiCurrentRow.

Inline OVER supports COUNT, numeric SUM/AVG, MIN/MAX, Rank, DenseRank and
RowNumber, with partitioning, ordering, ROWS and RANGE frames. Frame offsets
are nonnegative integer literals within Int32; ROWS literals must have Int32
type. RANGE offsets require one integer or floating order key. Functions with
the same specification share an RBO window operator, and repeated calls share
one result. Window inputs and grouped aggregates are ordinary typed columns;
the scalar model has no window macro. Window syntax currently uses the shared
grammar route. Generic queries default ordered windows to RANGE; data/scan
queries default to ROWS. Explicit pragmas override that choice, and named query
bindings retain the default from their definition site. The whole-partition
rewrite constructs native expressions, including checked optional unwrapping,
which executes and lowers directly and keeps failed constant checks lazy.

DateTime::Split, GetYear, MakeDate/MakeDatetime/MakeTimestamp, ShiftMonths and
ShiftYears have typed native operations for UTC Date/Datetime/Timestamp inputs.
Calendar values are scalar intermediates, including inside bindings and lambdas;
they must be converted to a temporal value before crossing a relational boundary.
Shifts accept required Int32 amounts or fitting integer literals. The reference
evaluator preserves month-end clamping, microseconds, range failures and the
existing narrow runtime's 12-bit year wraparound for large shifts. Terminal
lowering constructs fully typed runtime calls directly, with no annotation or
semantic rewrite pass. Timezone and wide-date types remain outside the profile.

Runtime String/Utf8-to-Date casts use the same bounded numeric date contract as
literal parsing; malformed or out-of-range dates produce NULL. DateTime's
IntervalFromDays/Hours/Minutes accept Int32 (including nullable inputs) or fitting
integer literals. They become ordinary widening, wrapping integer multiplication
and a checked Interval cast, with no extra expression kind or runtime UDF.

LIKE compiles to ordered byte searches and prefix/suffix checks, with no regex
object or runtime semantic lowering. It preserves YQL's pattern-dependent NULL
behavior and result types: forms handled by YQL's regex matcher return false
for NULL and invalid UTF-8; its simple string-operation forms propagate NULL.
The native binder inserts ordinary UTF-8 casts and Boolean operations for this
compatibility behavior. Repeated % wildcards, anchored suffixes and overlapping
literal pieces are covered by exhaustive short-pattern tests and runtime parity.

Scalar subqueries bind to typed relational results and use RBO's existing
cardinality check: no row yields NULL and more than one row raises a runtime
error. Scalar DAGs reference these results as columns; they contain no opaque
SQL or subquery execution node. EXISTS/NOT EXISTS also bind to ordinary required-Bool
columns. Existing RBO semi/anti joins handle direct filter conjuncts; the
Boolean-result rewrite handles OR predicates and native projection Maps. Native
expression composition is used throughout both rewrites. Populated tests cover
nested correlation, NULLs, duplicates, empty inputs, UNION ALL, and aggregate
subqueries under both null-safe join settings. The old frontend/RBO still fails
projected EXISTS cases; the old optimizer incorrectly returns false for EXISTS
of a global aggregate over an empty correlated input. Tests pin those existing
boundaries and require correct native results.

Single-column IN subqueries also bind to Boolean columns. Computed lookup keys
and common-type conversions are materialized once as ordinary native Maps;
nested IN expressions use the same mechanism. The existing RBO subplan rules
handle empty sets, nullable keys/items and correlated inputs. Generic queries
default to ANSI IN semantics; data/scan queries preserve their legacy default,
with either explicit pragma overriding it. IN lists obey the same setting.
Lookup filters retain native expressions through typing and lower only at the
physical boundary. Tests require correct native truth tables and separately pin
existing old-frontend failures for projected/computed lookups and inconsistent
legacy NOT IN filters; those older components are unchanged.

Table discovery includes nested query bodies. ORDER BY may reference a projected
subquery result through its alias, but direct scalar subqueries in sort keys
are excluded because the existing RBO backend fails their physical typing.
Outer columns receive typed local captures below their consumers; nested
subqueries forward captures through each caller scope. Aggregate arguments use
the input scope, and grouped callers expose their grouping keys. Ungrouped
outer references in a grouped projection or HAVING are explicitly unsupported,
matching YQL's grouping restriction. Existing RBO decorrelation and replicate
expansion construct native scalars without invoking YQL expression annotation.

Populated correlation tests compare the old frontend plus new RBO with native
execution under both null-safe join settings. They also pin existing legacy
differences: the old optimizer returns NULL for empty correlated COUNT groups
and fails ordered correlated LIMIT with a missing sort member. New RBO and
native preserve zero counts and correct ordered results. The old frontend also
rejects a grandparent reference through SELECT without FROM; native execution
of that case is checked against explicit table values, not counted as legacy
parity. These existing components are unchanged.

Named scalars, lambdas and queries may be rebound. References resolve to
definition IDs in one walk; lambda/query captures keep their original IDs
when a later statement reuses a name. Table discovery uses the same resolution
as binding. No environment snapshot is copied per definition or expansion.

Explicitly outside the profile:

- DML/DDL, USE, scripting effects, arbitrary pragmas/settings, views, temporary
  and external tables, table hints, sampling, and PostgreSQL syntax/types.
- Tuple IN subqueries; subqueries in JOIN ON, GROUP BY keys, ORDER BY expressions, LIMIT/OFFSET,
  scalar bindings or lambda bodies, JOIN USING, grouping sets,
  duplicate result names within a set operand, and stars over joins
  (which require YQL's column-name coalescing rules).
- Correlated set expressions containing INTERSECT ALL or EXCEPT ALL: the existing
  backend cannot decorrelate their numbering windows. Uncorrelated scalar/EXISTS/IN
  subqueries containing those operations execute natively.
- FULL JOIN predicates other than conjunctions of column equality keys. The
  existing RBO backend can ignore residual full-join filters; native-required
  rejects them before RBO, and automatic mode explicitly delegates the whole
  query to the legacy optimizer. The pre-existing backend is unchanged.
- Named/inherited windows, DISTINCT or filtered window functions, NULL treatment,
  frame exclusions, GROUPS frames other than whole partitions, and RANGE without
  ordering other than whole partitions. Windows inside scalar/EXISTS/IN subqueries,
  subqueries in window arguments/keys, and direct window calls in ORDER BY are
  excluded; ordering by a projected window alias is supported.
- UDFs, higher-order values, containers/structs/tuples, nested optionals.
- Mixed signed/unsigned promotion outside integer comparisons, mixed Float promotion, bare-NULL arithmetic, dynamic
  text parsing to types other than Date, and runtime casts from temporal inputs to non-temporal types.
  Decimal text with exponents or malformed text and Float/Double-to-Decimal
  conversions are excluded. Apart from integer multiplication factors, Decimal
  arithmetic and COALESCE require matching input types or explicit casts; their mixed-type rules differ from comparison
  and IF/UNION promotion.
- Float to Int32/Uint32/Int64/Uint64: the existing runtime rounds the upper
  integer bound to Float before checking, admitting an out-of-range value.
  These casts remain explicit unsupported features; no existing runtime fix is
  included. Double to integer and Float to 8/16-bit integer casts are supported.
- LIKE with _, escapes or dynamic patterns, ILIKE, and unverified function aliases. Invalid aliases are
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

Native constant folding visits shared scalar DAGs once per arena batch and
bounds generated string payloads. It preserves result types and keeps failing
runtime assertions deferred, including in conditional branches. The assertion
opcode also lowers directly to the backend's executable Ensure operation.

The internal StablePickle opcode produces the backend's exact scalar key
encoding, including nullable values and normalization of floating NaNs and
signed zero. It has native reference execution and constant folding, lowers
directly to MiniKQL, and is checked byte-for-byte for every native scalar type.
The existing decorrelation rules use these keys when equal-NULL join keys are
disabled. Fresh forwarding operators supply native column types directly to
scalar factories, without an additional plan annotation pass.

Column-store predicate pushdown keeps the original native scalar DAG through
optimization, pruning and copying. Only physical source conversion constructs
the backend storage program. Eligibility uses native types and operations;
it invokes no YQL annotation or peephole pipeline. Supported shapes include
comparisons, Boolean columns/logic, numeric arithmetic, IF/COALESCE, direct-column
null tests, parameters and simple string predicates. A supported leading
conjunction can move into storage while the remaining predicate stays in native
compute. Checked assertions prevent this split, preserving their error behavior.

Decimal/calendar operations, nontrivial casts, comparisons that consume the same
column twice, scalar-only Boolean branches, and predicates with external column
dependencies stay in compute. These are storage capability boundaries, not
frontend fallback. Native Boolean construction removes neutral operands such as
the initial `false OR` in an IN list before storage lowering. This avoids sending
a redundant scalar branch to the storage streaming-logic executor.

Pushed read limits are plain optional Uint64 values for both frontends. Literal
limit/top-N pushdown therefore needs no legacy expression in RBO; physical
conversion creates the backend literal. The OLAP tests compare 40 predicates on
populated tables across native/existing frontends with pushdown on/off, including
NULLs, integer extremes, NaN/infinity and Unicode. They also check empty/limited
results, nullable parameters, count-only reads, partial pushdown, result metadata,
and storage-filter/item-limit presence in explain output.
Validation covered all 79 standalone frontend and 395 RBO tests, with a final
arena-suite rerun after updating its neutral-expression fixture. All execution
tests passed, including the unchanged populated TPC-H and TPC-DS fixtures.

**Plan-quality limitation:** native range extraction and OLAP projection pushdown
are still missing, and native statistics use approximate selectivity. Legacy
scalar peepholes are not run on native scalars. Dual scalar/storage-program
representations still support the existing frontend during migration. Removing
this debt and establishing plan/execution parity remain required before broad
rollout. A faster frontend alone does not prove faster complete query execution.

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
schemas and prepends the same three scalar helpers as `RunPerf_YqlTest`.
`--bare` measures the SQL files alone. With the complete helper prelude, fast
and generated modes currently agree on these frontend routing counts:

| Suite | Native | Explicit legacy delegation |
| --- | ---: | ---: |
| TPC-H | 22/22 | 0 |
| TPC-DS | 65/99 | 34 |
| ClickBench | 37/43 | 6 |

There are no hard errors in this probe. Acceptance here means parsing, binding
and profile validation, not RBO optimization or execution. All 22 TPC-H
queries separately have native-required execution parity on populated column
tables. Fifteen TPC-DS queries also have populated native-required execution
checks (listed below); the other accepted TPC-DS and ClickBench queries still need
that validation. The report preserves
unsupported diagnostics per query, including unavailable statistical aggregates.

The set-operation increment newly admits Q8, Q49 and Q75. Q8 additionally has
populated execution parity across all three compilers, with duplicate zip
prefixes, strict customer-count thresholds, failed date/join predicates and NULL
profits. Fifty-one set queries run on row and column tables and explicitly distinguish
native success from existing translator/backend failures (ALL operations, mixed
set precedence, some mixed-schema sorts and bare-NULL grouping). No existing
compiler/backend defects are repaired by this increment.

Validation covers 80 standalone frontend and 397 RBO tests. The full run passed
475 of 477 checks; the two new compatibility-matrix tests were subsequently
rerun after pinning the old frontend's additional failures. The final native
bare-NULL sort normalization was checked with the set matrix and both populated
benchmark fixtures. Warm parse/bind scaling for 10/100/1,000 UNION ALL branches
was approximately 37/279/2,556 microseconds (three 100-iteration trials, concurrent
regression tests); these measurements exclude RBO and backend compilation.

## Validation and measurements

The full-join boundary passed 20 targeted compiler/integration tests. Row and
column fixtures pin the old frontend plus new RBO's incorrect residual-filter
matches, require native-required rejection, and verify explicit whole-query
delegation to the correct legacy optimizer. A following supported query stays
native. Missing join constraints and constrained CROSS JOINs are validation
errors, rather than accepted native queries. All 13 populated TPC-DS queries
still pass, all 164 parser routing decisions are unchanged, and the standalone
compiler has no YQL or MiniKQL symbols.

The window increment passed 74 standalone frontend tests, 45 native/RBO
tests and eight unchanged window regressions. Its populated matrix executes 160 native queries across row/column
tables, both window engines, and data/scan/generic APIs. It compares values and
metadata, with NULL partitions/keys/measures, ties, grouped inputs, empty and
sliding frames, Decimal aggregates, required inputs, outer joins, retained pragma
settings and an IN lookup above a window. The old frontend incorrectly leaves
SUM(required) non-nullable for potentially empty frames, so both existing
optimizer paths fail type checking in five cases. Tests pin those original
failures, obtain reference values by widening only the argument's nullability,
and require native to execute the original required-input query. No older
compiler or runtime is fixed. Separate RBO tests forbid YQL expression annotation
through native window expansion and copying; terminal unwrap is tested directly.

Fast and generated parser routes agree on all 164 corpus decisions with no
hard errors. Windows newly admit TPC-DS Q12, Q20, Q51, Q53, Q63, Q89 and Q98,
bringing frontend acceptance to 56/99; their populated benchmark execution is
verified for all seven. The standalone CLI still has no YQL or MiniKQL symbols.

Integer comparisons additionally admit TPC-DS Q6, Q9, Q34, Q41, Q44 and Q73,
and ClickBench Q28. Apart from Q41, these additions have frontend acceptance,
not yet populated benchmark execution coverage. Fast and generated parsers agree on all 164 routes
with no hard errors, and the standalone CLI has no YQL or MiniKQL symbols.
The comparison increment passed 102 compiler/expression/integration tests,
including all eight integer widths, constant folding, 1,920 direct runtime
evaluations and 3,072 parameterized result comparisons. Eight join kinds in both
operand orders pass explicit value checks on row and column tables, including
negative values, signed/unsigned limits, duplicate keys and NULLs. Native matches
the legacy optimizer for all these joins and the old frontend plus RBO for
ordinary/outer joins; that frontend's existing rejection of explicit SEMI/ONLY
syntax is pinned separately. All 22 populated TPC-H and 13 populated TPC-DS
queries were rerun successfully. No existing frontend or backend was fixed.

Fifteen TPC-DS queries (Q8, Q12, Q20, Q21, Q32, Q37, Q40, Q41, Q51, Q53, Q63, Q82,
Q89, Q92 and Q98) pass populated execution
against explicit expected values and the existing frontend plus new RBO, with
identical result values, column names/types/order and successful CBO invariants.
Q41 checks all eight correlated predicate branches, numeric manufacturer-range
boundaries, inner matches outside that range, duplicate products, NULL product
and manufacturer values, missing matches and individual invalid/NULL predicate
components. Its nine expected result rows and metadata match both existing
compiler paths; all fourteen populated queries pass together without fallback.
The three revenue/window queries also match the legacy optimizer exactly on
eight result rows each, with different totals for each sales channel, shared
classes across categories, NULL partitions and all-NULL groups, zero/negative
revenue, date boundaries, invalid dates and missing dimension rows.
The remaining window cases check cumulative SUM/MAX across a full outer join,
monthly versus quarterly grouping, compound and NULL partition keys, and the
strict 10% threshold. Q51 exposed an optional Boolean lift hiding a native join
equality. Native predicate splitting and equality inspection now look through
that value-preserving lift, including nested conjunctions, so both equalities
become join keys. The existing join backend is unchanged. The fix and expanded
fixtures passed all 50 tests across native expression/host suites and unchanged
window regressions, including all 22 populated TPC-H queries.
They retain the original SQL, full schema, helpers and benchmark settings.
Fixtures exercise date/price/quantity bounds, invalid and NULL dates, duplicate
sales, missing matches, strict correlated-average thresholds, NULL refunds and
negative net sales. All values also agree with the old optimizer. Q21, Q51,
Q53, Q63 and Q89 pin an existing metadata difference: the old optimizer alphabetizes SELECT * columns,
whereas new RBO preserves the derived table's projection order. Native retains
the existing RBO order; neither older compiler is changed.

The runtime Date/interval extension passed all 112 native tests. Native Date
parsing matches the existing runtime on 127,512 padded/unpadded date strings
across and beyond the supported range, plus malformed and binary suffix cases.
Direct MiniKQL tests keep casts dynamic and need no annotation pass. Host tests
compare 1,540 values and their types across nullable String/Utf8 inputs, interval
boundaries and 64-bit wrapping before interval validation. All 22 populated
TPC-H queries remain passing. TPC-DS frontend acceptance is now 49/99; Q21,
Q32, Q37, Q40, Q58, Q72, Q82, Q83 and Q92 were newly accepted by that increment.

The integer-factor extension passed all 109 native tests. It compares 1,600
arithmetic cases with the existing runtime across all eight integer widths,
four Decimal shapes, both operand orders and constant folding. Direct MiniKQL
tests require no annotation or semantic lowering. Parameterized SQL compares
900 values and their types with YQL, including NULLs, zero, full-width factors,
overflow, infinity and NaN.

All 22 checked-in TPC-H queries now pass populated native-required execution,
exact expected results, complete value/type/name parity against both legacy
paths, and RBO CBO invariants. Q11 includes positive inventory values and NULL,
zero and negative groups rejected by HAVING. Fast and generated grammar modes
agree on all 164 corpus routing decisions; the standalone CLI still contains
no YQL or MiniKQL symbols. That increment raised TPC-DS acceptance to 40/99,
with Q61, Q64 and Q93 newly accepted.

The calendar extension passed all 107 native tests. Direct MiniKQL execution
requires no graph annotation or peephole pass. Runtime parameter tests compare
660 calendar/cast values and their types with YQL, including NULLs, leap years,
month-end clamping, microseconds, nested shifts and range/wrap boundaries.
Additional tests check constant results, total cast metadata, rejection of
untyped NULL arguments, constant folding and scalar reconstruction. Both parser
routes agree on all 164 corpus routing decisions, with no hard errors.

The populated TPC-H fixture also passes Q4, Q5, Q6, Q7, Q9, Q10, Q12 and Q15
through all three compiler paths. It checks exact revenues, counts, profits,
years and tied maxima on 27 additional joined rows, with date boundaries,
nonqualifying rows and NULLs. All 18 queries retain matching values, metadata,
and successful CBO invariants on the unchanged benchmark SQL and schema.

Q8, Q14 and Q20 also pass all three paths. Eleven more joined rows exercise
Brazilian/non-Brazilian market shares, promotional date boundaries, and nested
IN with correlated SUM: duplicate qualifying supplier keys, equality excluded
by a strict comparison, and empty aggregate inputs. Their exact ratios and
supplier results are checked alongside all earlier cases, bringing populated
TPC-H execution coverage to 21/22 in that increment.

The LIKE extension passed all 103 native tests. The standalone matcher compares
all 121 patterns of up to four characters over `ab%` against all 31 strings over
`ab`, through both parser routes. Runtime parameter tests compare 360 values
and types with YQL, including NULL, Unicode, invalid UTF-8, newlines and embedded
zero bytes. Reconstruction tests also verify that native COALESCE retains a
required fallback through imports, type changes and constant folding.

The IN extension passed 124 selected native, decorrelation and lookup tests,
including 208 native populated executions across API defaults, explicit ANSI
settings, projected/filter predicates and both null-safe join settings. Fast
and generated parsing agree on all 164 corpus routes, with no hard errors.

The EXISTS extension passed 121 selected native and decorrelation tests,
including 22 native populated execution cases and unchanged legacy regressions.

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

The standalone CLI contains no YQL or MiniKQL symbols; the frontend's only parser runtime
dependency is generic ANTLR.

The Decimal extension passed all 79 native tests, plus 10 compiler checks after
correcting unsupported statistical-aggregate routing. Tests compare native
Decimal values with the existing arithmetic implementation, execute terminal
MiniKQL graphs without annotation, and compare runtime parameters with legacy
SQL values/types. All 22 checked-in TPC-H queries run with the complete schema,
helper prelude, maximum language version, and 16-partition column tables. Their
nonempty results match both the legacy optimizer and the old frontend with new
RBO. The fixture covers duplicate line items, nulls, date/quantity boundaries,
unmatched rows, and all three Q19 OR branches. Q3 revenues are explicitly checked
as 190 and 180; Q19 revenue is 750. Q2 checks tied European minimum costs,
excluded suppliers, NULL costs, and size/type filters. Q13 checks the distribution
of orders surviving NOT LIKE, including YQL's NULL-comment behavior, returning
counts/distribution `(0,10), (7,1), (2,1), (1,1)`. Q16 checks distinct suppliers
across parts, ordered complaint words and excluded brand/type/size values,
returning supplier counts 3 and 1. Q17 checks a Decimal
result of 1.43. Q18 checks sums above/equal to/below 300 and an all-NULL
group, returning orders 100, 102 and 104 with quantities 301, 302 and 301;
price/date ordering is checked with tied prices. Q21 checks duplicate qualifying late lines, competing suppliers,
unfinished orders and NULL dates, returning supplier counts 3 and 1. Q22 checks
an average qualifying account balance of 180 and excludes customers with orders,
returning country totals 200 and 250. Both RBO paths also satisfy the existing
benchmark invariant: all CBO trees are optimized, with at least one tree for the
join queries. The common-Decimal-type extension also passed all 79 native tests,
including runtime comparisons beyond double precision, IF/UNION result types,
and explicit rejection of unsupported mixed arithmetic and COALESCE.

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

After lexical binding support, the corresponding fast-mode medians were 6.18,
11.70, 13.68 and 232.44 microseconds with the same settings. Its native suite
passed 75 tests, including rebinding and captured-query table discovery.

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
successes. TPC-DS 17 and 44 are omitted from the success requirement; that
does not itself establish that either query fails. The migration target is all 121 queries with native-required compilation and
execution validation, plus previously working YQL read-query tests. Writes
and PostgreSQL are tracked separately. Pre-existing RBO/backend defects remain
outside this patch; failures caused by native translation or migration are fixed.

Scalar-subquery context checks also record existing limitations with
`YqlSelect=force`: the legacy optimizer leaves a sublink inside a SUM argument
unlowered, while the existing RBO path executes that case. Direct subqueries in
ORDER BY fail physical typing in the existing RBO backend; subqueries in GROUP
BY keys are rejected by the existing SQL translator. The native profile excludes
the latter two forms explicitly. These existing components are not patched.
