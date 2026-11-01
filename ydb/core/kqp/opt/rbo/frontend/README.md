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
HAVING, GROUPING SETS/ROLLUP/CUBE and GROUPING masks, inline windows, DISTINCT, UNION/INTERSECT/EXCEPT (including ALL), ordering, limits, scalar/EXISTS/IN subqueries with correlation,
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
Aggregates are COUNT, SUM, AVG, MIN, MAX, SOME, and sample standard deviation
(`STDDEV`, `STDDEV_SAMPLE`, `STDDEV_SAMP`, `STDDEV_S`, and their spellings without
underscores). Standard deviation accepts integer/Float/Double inputs and returns
Double with ordinary aggregate nullability. Singleton groups produce NaN;
empty/all-NULL inputs produce NULL. The RBO handoff maps the semantic aggregate
directly to the existing sample-deviation backend trait. Partial outputs carry
the actual `(mean, count, M2)` state: three required Doubles, with an optional
tuple only when the input is nullable. The final aggregate checks that state
shape and returns Double with ordinary aggregate nullability. The backend
consumes the typed state without changing its numerical algorithm. Decimal arguments,
window forms and mixed aggregate combinations containing DISTINCT explicitly delegate.
`Math::IsNaN` on typed integer/Float/Double inputs uses native scalar IEEE
inequality, retaining NULLs; it needs no UDF or semantic lowering pass.
Multiple DISTINCT aggregates can be combined with ordinary COUNT, SUM, MIN,
MAX, SOME and AVG. AVG carries a typed `(sum, count)` state and preserves its
original scalar result type, including Decimal precision. DISTINCT groups the
original input values before conversion into the accumulator type.
Computed GROUP BY keys require explicit
aliases (`GROUP BY expression AS key`, then `SELECT key`) or
`PRAGMA YqlSelectAllowUnnamedGroupByExpr`. Its enable/disable state is captured
when a query is defined, including named queries. Unnamed keys are materialized
once and referenced by their canonical expression; internal column names are
not exposed as SQL bindings. Scan queries require one result.

Both parsers expand grouping syntax into bounded key lists, preserving duplicate
sets. Binding assigns outward key/result IDs and nullable types once; GROUPING
masks use ordinary Uint64 arithmetic over indicator columns. The RBO handoff
emits aggregate branches, native NULL/optional/indicator maps and one union,
sharing the input through Replicate. DISTINCT expands independently in each
branch. Ordinary keys common to every set retain required types. Bare NULL
keys in grouping sets require an explicit scalar cast; GROUP BY modifiers and
hopping-window grouping remain outside the profile. ORDER BY expressions can
refer to output aliases; their map is placed above the projection map.

The grouping matrix exercises 24 query forms on populated and empty row/column
tables: duplicate sets/keys, composite keys, common required keys, nullable
measures, mixed DISTINCT, HAVING, windows and alias shadowing. Known older-path
failures are asserted separately. The old optimizer drops duplicate empty sets;
the old frontend with RBO combines DISTINCT states incorrectly across rollup
levels and leaves required SUM non-null on empty input. Tests pin these existing
results without changing either implementation. Native preserves duplicate
sets and computes each level independently. The converter invariant test forbids
YQL annotation and verifies native expressions through pruning, DISTINCT
expansion and Replicate copying.

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
Unicode::ToUpper/ToLower accept Utf8 values and valid UTF-8 string literals,
preserve optionality, and use the runtime's simple Unicode codepoint mapping.
Native evaluation uses the generic Unicode library; terminal backend lowering
calls the typed Unicode runtime functions directly.
The SQL aliases tinyint, smallint, int/integer, bigint and varchar resolve to
canonical scalar types in declarations, casts and literal constructors.
Integer comparisons preserve each operand's width and signedness and compare
their mathematical values, including Int64 against Uint64. They return nullable
Bool when either input is nullable; no common-type casts are needed. Arithmetic,
IF/COALESCE and set operands use SQL numeric rank: wider integers win, signed
wins at equal width, then Float and Double. Implicit integer conversions wrap
modulo the target width; explicit CAST remains checked. Float/integer promotion
converts directly to Float, avoiding intermediate Double rounding. All these
conversions preserve input NULLs and lower directly to a typed runtime Convert.
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
Decimal division accepts an integer divisor without widening or rescaling
either operand. Integer division uses the existing runtime's coefficient
rounding, including its negative-divisor quirk: non-ties truncate, while exact
ties round to even. The native evaluator and constant folder preserve that
behavior; the backend lowers directly to DecimalDiv. No old runtime is changed.
Decimal COALESCE permits precision widening when all Decimal operands retain
the same scale. It preserves coefficients and each alternative's nullability,
including 35-digit SUM results with a narrower zero fallback. Different scales
and mixed integer/Decimal alternatives still require explicit casts.
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

Uncorrelated scalar/EXISTS/IN subqueries may contain windows, including windows
inside derived tables or named query bindings. The binder checks actual external
column captures after binding the subquery. Correlated subqueries containing
windows remain explicitly unsupported because the existing backend cannot
decorrelate them; merely having an enclosing query does not imply correlation.

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
  scalar bindings or lambda bodies, JOIN USING,
  duplicate result names within a set operand, and join-star projections with
  duplicate output names (which require column-name coalescing). Plain and
  qualified stars over joins with unique output names are supported.
- Correlated set expressions containing INTERSECT ALL or EXCEPT ALL: the existing
  backend cannot decorrelate their numbering windows. Uncorrelated scalar/EXISTS/IN
  subqueries containing those operations execute natively.
- FULL JOIN predicates other than conjunctions of column equality keys. The
  existing RBO backend can ignore residual full-join filters; native-required
  rejects them before RBO, and automatic mode explicitly delegates the whole
  query to the legacy optimizer. The pre-existing backend is unchanged.
- Named/inherited windows, DISTINCT or filtered window functions, NULL treatment,
  frame exclusions, GROUPS frames other than whole partitions, and RANGE without
  ordering other than whole partitions. Windows inside correlated scalar/EXISTS/IN subqueries,
  subqueries in window arguments/keys, and direct window calls in ORDER BY are
  excluded; ordering by a projected window alias is supported.
- Arbitrary UDFs, higher-order values, containers/structs/tuples, nested optionals.
- Bare-NULL arithmetic, dynamic
  text parsing to types other than Date, and runtime casts from temporal inputs to non-temporal types.
  Decimal text with exponents or malformed text and Float/Double-to-Decimal
  conversions are excluded. Apart from integer multiplication factors and divisors, Decimal
  arithmetic requires matching input types or explicit casts. COALESCE additionally
  permits same-scale Decimal precision widening; its other mixed-type rules remain
  outside the profile, independently of comparison and IF/UNION promotion.
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

SQL `Unwrap(value[, message])` uses the existing executable unwrap opcode. Its
optional String/Utf8 message must be non-null and is evaluated only when the
value is NULL; required values are returned unchanged, as in YQL. Rewrites
preserve both arguments and failing checks remain deferred. Bare NULL would
produce Void in YQL and is explicitly outside the native scalar profile.

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

Native filter statistics now consume the existing column histograms, frequency
sketches and key metadata directly from the typed expression DAG. The visitor
in `kqp_rbo_statistics.cpp` estimates each reachable node once, including shared
subexpressions, and allocates no YQL graph. Both ordinary filters and pushed
read predicates use it. The same visitor identifies which columns need sketches
or histograms for the existing asynchronous statistics loader. Positive
column-equality conjuncts also request join-key histograms and available tuple
histograms; OR/NOT branches do not invent join keys. Source lineage keeps
aggregate results and computed columns from borrowing base-table statistics.
Sketch probes retain the column's storage width,
including signed/unsigned 64-bit values beyond Double's exact range, temporal
units and embedded string bytes. Source statistics cross optional/identity
conversions only; other casts and computed expressions use heuristics.

Equality uses a frequency sketch when available, otherwise the existing key
or non-key default. Comparisons use typed histogram boundaries when available.
Nested single-column intervals intersect under AND and overlapping intervals
merge under OR; empty intervals and endpoint inclusivity are retained. Literal
NULL/Boolean predicates, Boolean combinations, COALESCE, null checks, CASE and
simple string predicates have explicit estimates. Parameters use heuristics;
statistics never evaluate them or runtime assertions during compilation.

These are cost estimates, not a second semantic evaluator. Decimal boundaries,
value-changing casts, correlations across columns and compound disjoint interval
sets still use heuristics. Without a measured null fraction, nullable column
null checks use a 10% default; other predicates assume no unknown null fraction.
Native and legacy estimation do not promise identical plans. Tests check storage
widths, key defaults, interval endpoints, NULLs, a 20,000-node shared DAG and
agreement between ordinary and pushed filters without native-to-YQL lowering.
The complete 428-test RBO suite passed, including unchanged populated execution
of all 22 TPC-H and 41 TPC-DS queries across the three compiler paths.

Native row-table range extraction reads the typed DAG directly. Equality
conjuncts pin a contiguous key prefix, optionally followed by one interval;
selected predicates are removed and remaining predicates retain their order.
Bounds are literals or required parameters of the exact key type. Integral
literal widths and signedness may change only when the mathematical value fits.
NULL keys are excluded from upper-only comparisons with an explicit exclusive
NULL lower bound. Nullable parameters, out-of-domain literals, value-changing
column casts, floating/Decimal keys and disjunctions remain filters. Any checked
expression prevents extraction rather than risking suppression of its error.
The read stores native bound expressions, exposes them to ordinary expression
rewrites and emits `KqlKeyRange` only at physical conversion. Explain shows each
bound's values and inclusivity. The complete original predicate survives for
statistics and fetched-row filtering when a lookup join replaces the range scan.
Tests forbid YQL annotation during extraction, check copying/pruning and a
20,000-node shared DAG, and compare 102 populated query executions across the
native frontend and both existing compiler paths. Native plans assert range
versus full scans for 22 cases, including composite and NULL keys, descending
limits with residual filters, contradictory intervals, exact 64-bit boundaries
and prepared-parameter reuse. These are correctness checks, not a compilation
speedup measurement.

**Plan-quality limitation:** native multi-range extraction, column-store ranges,
range-based index selection, lookup point prefixes and OLAP projection pushdown
remain unfinished. Legacy scalar peepholes are not run on native scalars. Dual scalar/storage-program
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
| TPC-DS | 99/99 | 0 |
| ClickBench | 40/43 | 3 |

There are no hard errors in this probe. Acceptance here means parsing, binding
and profile validation, not RBO optimization or execution. All 22 TPC-H
queries separately have native-required execution parity on populated column
tables. Forty-one TPC-DS queries also have populated native-required execution
checks (listed below); the other accepted TPC-DS and ClickBench queries still need
that validation. The report preserves
unsupported diagnostics per query, including unavailable statistical aggregates.

Join-star expansion with unique output names admits Q10, Q35, Q69 and Q88.
Q88 has populated execution coverage; the other three have frontend coverage.
Sample standard deviation and `Math::IsNaN` additionally admit Q17/Q39 at the
frontend. Both have populated execution coverage below.
Same-scale Decimal COALESCE widening additionally admits Q78, with populated
execution coverage below. Typed AVG states additionally admit TPC-DS Q28 and
ClickBench Q10 at the frontend. Q28 has populated execution coverage below;
ClickBench Q10 execution is still pending.

Integer Decimal division and Unicode case conversion newly admit TPC-DS Q4,
Q24, Q54 and Q66. Q24, Q54 and Q66 have populated execution parity below; Q4
still needs execution validation. The original Q4 web-sales branch emits
`sale_type = 's'`, while its final join requires `'w'`, so that unchanged query
is logically empty for every dataset. This pre-existing SQL issue is not repaired
or used as evidence of nonempty-result arithmetic coverage.

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

SQL Unwrap additionally admits Q38 and Q87. Both execute unchanged on populated
tables through all three compiler paths, with exact counts of four intersected
and three remaining name/date keys. The fixture covers duplicate sales, NULL
and empty names, invalid dates mapped to the epoch, date bounds, missing join
keys, and matches in only one competing channel. All 17 populated TPC-DS
queries pass together. Validation also passed 81 standalone frontend tests and
22 native RBO/frontend regression tests, plus the selected reused-session scalar
and runtime-Unwrap tests. Both parsers agree on all 164 final corpus routes.
The scalar checks caught and fixed a native validation error rejecting
`CAST(NULL AS String/Utf8)`; no older compiler or backend was changed.

Mixed DISTINCT additionally admits TPC-DS Q16, Q94 and Q95, and ClickBench Q23.
The existing RBO expansion retains its partial-aggregation/union/final-aggregation
plan. Native padding uses typed NULLs and optional lifts; COUNT's empty-input zero
and grouped required results use explicit native COALESCE/Unwrap expressions.
No YQL expression annotation is needed during expansion, pruning or copying,
and output types remain unchanged before terminal lowering. AVG and sample-deviation
partial outputs expose their actual tuple types, while final outputs retain the scalar type.
The terminal builder uses that scalar type to finish Decimal AVG without
reconstructing its original precision from an earlier plan.
Forty sample-deviation handoff cases check required/optional numeric inputs,
global and grouped aggregation, rollup and duplicate grouping sets. Partial-state
forwarding, pruning and replicate copying preserve native expressions and result
types with YQL annotation forbidden. Eleven runtime query forms compare sample
deviation alone and alongside AVG, Decimal AVG, COUNT and SUM on row/column
tables through all three compiler paths (66 executions). These retain the earlier queries and
add mixed-state cases with exact average/count/sum checks, including empty and
all-NULL inputs. Mixed DISTINCT with sample deviation remains explicitly unsupported.

The expression arena can carry internal flat tuple states with scalar fields.
Shapes are interned per query; copying between arenas imports the shape ID.
NULL padding, optional lifting, lazy IF/COALESCE, reference execution and direct
MiniKQL lowering retain the field types. Folding counts strings inside tuple
values against the existing byte budget. SQL container syntax, nested tuples,
tuple comparison, arithmetic and serialization remain explicitly unsupported.
The extension passes all 99 standalone tests and 27 selected RBO/host tests,
including direct MiniKQL execution, mixed DISTINCT on row/column tables, and the
unchanged populated 22 TPC-H / 37 TPC-DS checks across all three compiler paths.
The Linux build measures 8-byte type handles and 128-byte expression nodes; the
standalone CLI still contains no YQL or MiniKQL symbols.
The AVG handoff separately passes 99 frontend and 13 RBO/host tests, including
all currently populated benchmarks, grouping sets, windows and correlation.
Four-partition tests preserve DISTINCT on Int64 values above double precision:
`2^53`, `2^53+1`, `-2^53` must average to `2^53/3`, whereas prematurely deduplicating
after conversion would give zero. Required/nullable, all-NULL and empty inputs,
multiple AVG states and original Decimal precisions are compared across all
three compiler paths.

Eighty mixed-DISTINCT queries run on both row and column tables, checking required
and nullable inputs, multiple distinct states, computed arguments, Decimal values,
NULL keys, all-NULL groups and empty input. Values and metadata match the old
optimizer. The old frontend plus RBO also matches except for its existing rejection
of computed DISTINCT arguments, which is pinned without changing that frontend.
All 20 populated TPC-DS queries pass through all three compilers. Q16/Q94 each
return three unreturned orders with shipping cost 3 and profit 9; Q95 returns two
returned orders with shipping cost 5 and profit 3. Duplicate warehouses/returns,
NULL measures, both shipping-date bounds, invalid/missing dates and failed dimension
joins are covered with the original SQL, full schemas, helpers and settings.
The broad run passed 145/147 checks; the two new matrix failures were the old
computed-DISTINCT rejection. After pinning it and making native result restoration
explicit, all 28 targeted checks passed, including both populated benchmarks and
unchanged aggregation regressions. All 82 standalone frontend tests pass; both
parsers agree on all 164 corpus decisions with no hard errors.

## Validation and measurements

The same-scale Decimal COALESCE increment passed all 97 standalone frontend
tests and four selected runtime/host tests. All three compiler paths agree on
exact parameterized values and metadata for nullable/required alternatives,
operand order, three arguments, 35-digit limits, infinities and NaNs. A required
first alternative keeps an unreachable failing Unwrap unevaluated. Native
evaluation and constant folding preserve coefficients at scales 0, 2 and 3.
All 22 populated TPC-H and 36 populated TPC-DS regressions pass. With the corpus
probe's schema-nullability handling corrected, both parsers agree on all 164
routes and diagnostics. At that stage Q78 bound natively and Q28 was the last
TPC-DS delegation; the typed AVG handoff subsequently removed that boundary.

The sample-deviation increment passed all 95 standalone frontend tests and five
selected host tests. Row/column fixtures compare all three compiler paths on
grouped/global/sole-DISTINCT aggregates, required and optional integer inputs,
Float/Double conversions, singleton/all-NULL/empty groups, infinity and NaN.
Values, names, types and column order are checked explicitly; floating results
use a 1e-12 tolerance. Row keys span four uniform partitions. Direct `IsNaN`
parameter tests compare exact values and metadata for Float/Double, integer
limits, NULLs, signed zero, infinities and NaNs. All 22 populated TPC-H and
35 populated TPC-DS regressions pass. Fast/generated parsers agree on all
164 routes and diagnostics, and the standalone CLI has no YQL/MiniKQL symbols.

The join-star increment passed all 93 standalone frontend tests and six selected
host tests. Nine join kinds on both row and column storage check values, output
names/order and nullability, including qualified and reversed stars, duplicate
keys and unmatched/NULL keys. Duplicate output names remain explicitly
unsupported. The legacy optimizer supplies comparison coverage; the existing
frontend into new RBO's rejection of qualified stars and SEMI/ONLY syntax is
pinned without changing it. All 22 populated TPC-H and 35 populated TPC-DS
queries pass, and fast/generated parser routes and diagnostics agree on all
164 corpus queries with no hard errors.

The Decimal-divisor and Unicode-case increment passed all 92 standalone frontend
tests and nine selected runtime/host tests. Decimal arithmetic is compared with
the existing runtime across all eight integer widths and four Decimal shapes,
including negative-divisor rounding, NULLs, overflow, infinity and NaN. Parameter
tests compare values and metadata with both existing compiler paths. Unicode
tests check required/optional inputs, invalid String-to-Utf8 casts, supplementary
codepoints, combining characters and embedded zero bytes. Terminal runtime
tests require no expression annotation or semantic lowering pass. All 22
populated TPC-H and 33 populated TPC-DS queries pass together with their original
SQL, schemas, helpers and settings.

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

Forty-one TPC-DS queries (Q8, Q12, Q16, Q17, Q20, Q21, Q22, Q23, Q24, Q25, Q27, Q28, Q29, Q32, Q36, Q37,
Q38, Q39, Q40, Q41, Q47, Q51, Q53, Q54, Q57, Q62, Q63, Q66, Q70, Q78, Q82, Q86, Q87, Q88, Q89, Q90,
Q92, Q94, Q95, Q98 and Q99) pass populated execution
against explicit expected values and the existing frontend plus new RBO, with
identical result values, column names/types/order and successful CBO invariants.
Q17 checks three ordered groups across its store-sales/returns/catalog join,
including catalog fanout, every date-quarter filter, mismatched/NULL keys and
missing dimension rows. The ten-row group has exact means 1/2/5, sample
standard deviations 2/4/0 and coefficients 2/2/0. Other groups distinguish
singleton NaNs sanitized by the query's local lambda, all-NULL measures, and
NaN from division by a zero mean. Counts, values and result metadata/order
match both existing compilers. All 39 queries pass together with the previous
38 expectations unchanged and successful CBO checks; the original SQL, schema,
helpers and settings are preserved.
Q25 and Q29 each check four ordered groups through the same three-fact join,
with their different original date predicates and Decimal/integer sums. Catalog
fanout, equal store names on distinct IDs, NULL group attributes, all-NULL
measures, negative and zero totals, missing/mismatched keys, and all three date
filters have explicit expected results. Q25 includes a 399999.96 sum from
Decimal(7,2) inputs, with Decimal(35,2) output; Q29 includes cancellation of large
Int64 quantities. All 41 queries pass together through all three compiler paths
with the previous 39 expectations unchanged, exact metadata/order and CBO checks.
Q28 checks all eighteen columns from its six cross-joined scalar aggregates.
Expected means are 9.75, 57.12, 42.10, -16593.58, 83.50 and 16682.75, each retaining
Decimal(7,2) optional metadata. Counts distinguish duplicate and NULL prices.
The fixture covers every quantity range and OR arm, inclusive and excluded
bounds, NULL quantity, both rounding-tie directions, and a sum exceeding the
input Decimal precision while the mean fits. Each existing compiler's SELECT *
column order is pinned. All 38 queries pass together through all three compiler
paths with the previous 37 expectations unchanged and all CBO invariants intact.
Q78 adds five ordered channel-comparison results with exact quantities, Decimal
costs/prices and integer ratios. It checks all three return anti-joins (including
wrong item/order keys), missing competing channels, all-NULL amounts, NULL store
quantity, and a zero denominator despite a positive channel passing the WHERE.
Wrong customer/item/year keys, NULL customers, nonpositive/NULL channel quantities
and missing dates are excluded. All 37 queries pass together through all three
compiler paths with the previous 36 expectations unchanged, original SQL,
schemas, helpers and settings, exact metadata/order and successful CBO checks.
Q39 checks both original result sets against exact expected values, with three
rows in the first and two in the second. Inventory groups have sample deviations
2, 4, 6 or 8 and coefficients 2 or 4/3. Exclusions cover zero/negative means,
the exact coefficient-one threshold, singleton/all-NULL groups, missing months
and dimensions, and wrong/NULL years. A NULL warehouse name and a shared item
across warehouses exercise the grouping and self-join keys. All 36 queries pass
together through all three compiler paths, with the previous 35 expectations
unchanged and the original SQL, schemas, helpers and settings preserved.
Q24 checks Unicode country comparison, both return-join keys, NULL dimensions
and amounts, the strict 5% threshold, and grouped amounts that collapse in the
outer aggregation; its three paid totals are 30, 50 and 21. Q54 checks duplicate
customers across both channels, distinct scalar month queries, date bounds,
address-based store matching and Decimal-to-integer segment rounding. Its five
segment/count results are `(NULL,1), (-2,1), (0,1), (2,3), (4,1)`.
Q66 adds seven warehouse groups with explicit values for all twelve monthly
sales, per-area and net-paid measures. It checks channel-specific fields,
quantity multiplication, division before the outer sum, rounding ties, negative,
zero and NULL areas, all-NULL amounts, identical attributes on distinct warehouse
IDs, and both time and shipping filters. All 34 queries pass together through
all three compilers; the previous 33 expectations remain unchanged.
Q88 adds eight cross-joined scalar counts with expected values 1 through 8.
It checks half-hour boundaries, all three household branches, wrong stores and
NULL/missing dimension rows. All 35 queries pass through all three compiler
paths, preserving the original SQL, schemas, helpers and settings, and all
previous 34 expectations. The test pins the legacy optimizer's alphabetical
star-column order separately and requires native to match the existing RBO order.
Q47/Q57 exercise mixed signed/unsigned rank offsets and adjacent-month joins,
with duplicate sales, missing ranks, NULL keys, zero/NULL means and exact
deviation thresholds. Q90 checks the count ratio 5/(1+3)=1.25, including both
hour/character bounds and missing/NULL dimension rows. All 34 pass together
through all three compiler paths with the original SQL, schemas and helpers.
Q22, Q27, Q36 and Q86 check four-key inventory rollup, demographic-filtered
averages, hierarchy margins and sales ranks. Their independent expected values
cover real NULL keys versus subtotal NULLs, all-NULL measures, shared prefixes,
negative/zero amounts, date boundaries and missing dimension rows. All four
match both existing compiler paths, with the original SQL and full schemas.
Q70 adds an uncorrelated window inside IN and state/county hierarchy ranks. Its
13 rows match explicit totals and ranks on all three compiler paths, including
NULL states excluded by IN, NULL counties, all-NULL measures and month bounds.
Separate scalar/EXISTS/IN window-subquery tests cover empty inputs, duplicate
values, named query bindings and row/column storage. Existing RBO's projected
EXISTS failure is pinned separately; native matches the legacy optimizer.
Q62/Q99 cover all shipping-duration bucket bounds, duplicate computed warehouse
keys, NULL deltas and grouping fields, date bounds and missing dimensions. Q23
checks both result statements, a total of 87 and six customer/channel totals,
strict item-frequency and 95% revenue thresholds, all-NULL inputs and distinct
year filters for maximum versus qualifying-customer revenue. All three retain
the original SQL and match both older compiler paths in values, metadata and
ordering. The computed-grouping matrix also pins an existing RBO failure for
HAVING on a computed key; native matches the old optimizer on that case.
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
