#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/parser.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/resource/resource.h>
#include <algorithm>
#include <type_traits>

using namespace NKikimr::NKqp::NFrontend;

static_assert(!std::is_constructible_v<TCompiledProgram, TBoundProgram>);
static_assert(!std::is_default_constructible_v<TCompiledProgram>);
static_assert(std::is_same_v<decltype(std::declval<TCompiledProgram>().Get()), const TBoundProgram&>);

namespace {
struct TKeyword { std::string_view Word; unsigned Identifiers; };
#include <ydb/core/kqp/opt/rbo/frontend/grammar_keywords.inc>

TCatalogTable Catalog(std::string_view name, TSourcePosition) {
    return {std::string(name), {{"id", {ETypeKind::Uint64}}, {"value", {ETypeKind::Int32, true}}}};
}

void AssertParserAgreement(std::string_view sql, bool ansi = false) {
    TCompilerSettings fast;
    fast.Parsing.AnsiQuotes = ansi;
    std::optional<TParsedQuery> parsed;
    try { parsed = ParseNative(sql, fast); } catch (const TFrontendError&) {}
    std::optional<TParsedQuery> grammar;
    try { grammar = ParseGrammar(sql, fast.Parsing); } catch (const TFrontendError&) {}
    UNIT_ASSERT_VALUES_EQUAL_C(bool(parsed), bool(grammar), sql);
    if (parsed) UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(*parsed), SyntaxFingerprint(*grammar), sql);
}
}

Y_UNIT_TEST_SUITE(KqpNativeCompiler) {
    Y_UNIT_TEST(SampleStandardDeviationHasExplicitTypesAndBoundaries) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* alias : {"STDDEV", "STDDEV_SAMPLE", "stddev_samp", "stddev_s", "StdDevSample", "StdDevSamp", "StdDevS"}) {
                for (const auto* type : {"Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64", "Float", "Double"}) {
                    for (const bool nullable : {false, true}) for (const bool grouped : {false, true}) {
                        const auto catalog = [&](std::string_view name, TSourcePosition) {
                            auto valueType = ParseType(type); valueType.Nullable = nullable;
                            return TCatalogTable{std::string(name), {{"k", {ETypeKind::Int32}}, {"v", valueType}}};
                        };
                        const auto sql = std::string("SELECT ") + alias + "(v) AS s FROM t" + (grouped ? " GROUP BY k" : "");
                        const auto compiled = CompileNative(sql, catalog, settings);
                        const auto& program = compiled.Program->Get();
                        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Double, nullable || !grouped}));
                        for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Aggregate)
                            UNIT_ASSERT_VALUES_EQUAL(relation.Aggregates[0].Function, "stddev_samp");
                    }
                }
            }
            for (const auto* sql : {"SELECT STDDEV_SAMPLE(DISTINCT value) FROM t", "SELECT STDDEV(value), AVG(value) FROM t",
                    "SELECT value, STDDEV(id) FROM t GROUP BY ROLLUP(value)",
                    "$f = ($x) -> { RETURN CASE WHEN Math::IsNaN($x) THEN NULL ELSE $x END; }; SELECT $f(STDDEV(value)) FROM t"})
                UNIT_ASSERT(!CompileNative(sql, Catalog, settings).UsesLegacy());
            settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {"SELECT STDDEV(Decimal('1', 3, 2))", "SELECT STDDEV(value), COUNT(DISTINCT id) FROM t",
                    "SELECT STDDEV(DISTINCT value), COUNT(*) FROM t",
                    "SELECT STDDEV_POP(value) FROM t", "SELECT VARIANCE(value) FROM t"})
                UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            // Window syntax uses generated-parser dispatch; it is outside
            // both this aggregate feature and handwritten verification.
            if (parser != EParser::Verify)
                UNIT_ASSERT(CompileNative("SELECT STDDEV(value) OVER () FROM t", Catalog, settings).UsesLegacy());
        }
    }

    Y_UNIT_TEST(JoinStarsRequireUniqueOutputNames) {
        const auto catalog = [](std::string_view name, TSourcePosition) {
            const std::string prefix(name);
            return TCatalogTable{prefix, {{prefix + "id", {ETypeKind::Int32}},
                {prefix + "value", {ETypeKind::Utf8, true}}}};
        };
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* kind : {"CROSS", "INNER", "LEFT", "RIGHT", "FULL", "LEFT SEMI", "RIGHT SEMI", "LEFT ONLY", "RIGHT ONLY"}) {
                const auto join = std::string(kind);
                const auto sql = "SELECT * FROM a AS l " + join + " JOIN b AS r" +
                    (join == "CROSS" ? "" : " ON l.aid = r.bid");
                const auto compiled = CompileNative(sql, catalog, settings);
                const auto& columns = compiled.Program->Get().Results[0].Columns;
                const bool left = join != "RIGHT SEMI" && join != "RIGHT ONLY";
                const bool right = join != "LEFT SEMI" && join != "LEFT ONLY";
                UNIT_ASSERT_VALUES_EQUAL_C(columns.size(), 2 * (left + right), sql);
                std::size_t index = 0;
                for (const auto side : {std::pair{"a", left}, std::pair{"b", right}}) {
                    if (!side.second) continue;
                    const bool outer = join == "FULL" || (side.first == std::string("a") ? join == "RIGHT" : join == "LEFT");
                    UNIT_ASSERT_VALUES_EQUAL_C(columns[index].Name, std::string(side.first) + "id", sql);
                    UNIT_ASSERT_C(columns[index++].Type == (TType{ETypeKind::Int32, outer}), sql);
                    UNIT_ASSERT_VALUES_EQUAL_C(columns[index].Name, std::string(side.first) + "value", sql);
                    UNIT_ASSERT_C(columns[index++].Type == (TType{ETypeKind::Utf8, true}), sql);
                }
            }
            const auto reversed = CompileNative("SELECT r.*, l.* FROM a AS l LEFT JOIN b AS r ON l.aid = r.bid", catalog, settings);
            const auto& columns = reversed.Program->Get().Results[0].Columns;
            UNIT_ASSERT_VALUES_EQUAL(columns[0].Name, "bid");
            UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT_VALUES_EQUAL(columns[2].Name, "aid");
            UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Int32}));
            for (const auto* sql : {
                    "SELECT * FROM a AS l JOIN a AS r ON l.aid = r.aid",
                    "SELECT l.*, r.* FROM a AS l JOIN a AS r ON l.aid = r.aid",
                    "SELECT l.*, l.aid AS aid FROM a AS l JOIN b AS r ON l.aid = r.bid"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog, settings), TUnsupportedFeature, sql);
            for (const auto* sql : {
                    "SELECT missing.* FROM a AS l JOIN b AS r ON l.aid = r.bid",
                    "SELECT r.* FROM a AS l LEFT SEMI JOIN b AS r ON l.aid = r.bid"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, catalog, settings), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(UnicodeCaseHasUtf8InputsAndResults) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("DECLARE $x AS Utf8?; SELECT Unicode::ToUpper($x), Unicode::ToLower(Utf8('AZ')), Unicode::ToUpper('a'), Unicode::ToLower(NULL)", Catalog, settings);
            const auto& columns = compiled.Program->Get().Results[0].Columns;
            for (std::size_t i = 0; i < columns.size(); ++i)
                UNIT_ASSERT(columns[i].Type == (TType{ETypeKind::Utf8, i == 0 || i == 3}));
            for (const auto* sql : {"DECLARE $x AS String; SELECT Unicode::ToUpper($x)", "SELECT Unicode::ToLower(1)",
                    "SELECT Unicode::ToUpper()", "SELECT Unicode::ToLower('a','b')"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(UnnamedGroupingCapturesItsDefinitionScope) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 AS k, COUNT(*) FROM t GROUP BY value + 1 HAVING value + 1 > 0 ORDER BY value + 1",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 AS k, COUNT(*), GROUPING(value + 1) FROM t GROUP BY ROLLUP(value + 1)",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; $q = (SELECT value + 1 AS k FROM t GROUP BY value + 1); PRAGMA DisableYqlSelectAllowUnnamedGroupByExpr; SELECT k FROM $q",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT k FROM (SELECT value + 1 AS k FROM t GROUP BY value + 1) AS q"})
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            const auto duplicate = CompileNative("PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 FROM t GROUP BY value + 1, value + 1", Catalog, settings);
            for (const auto& relation : duplicate.Program->Get().Relations)
                if (relation.Kind == ERelation::Aggregate) UNIT_ASSERT_VALUES_EQUAL(relation.GroupKeys.size(), 1);
            for (const auto* sql : {
                    "SELECT value + 1 FROM t GROUP BY value + 1",
                    "$q = (SELECT value + 1 AS k FROM t GROUP BY value + 1); PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT k FROM $q",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; PRAGMA DisableYqlSelectAllowUnnamedGroupByExpr; SELECT value + 1 FROM t GROUP BY value + 1",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr='false'; SELECT 1"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TUnsupportedFeature, sql);
            for (const auto* sql : {
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT value FROM t GROUP BY value + 1",
                    "PRAGMA YqlSelectAllowUnnamedGroupByExpr; SELECT expr2 FROM t GROUP BY value + 1"}) {
                try { CompileNative(sql, Catalog, settings); UNIT_FAIL(sql); }
                catch (const TUnsupportedFeature&) { UNIT_FAIL("invalid grouped column delegated"); }
                catch (const TFrontendError&) {}
            }
        }
    }

    Y_UNIT_TEST(GroupingSetsBindNullableOutputsAndExecutableMasks) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto compiled = CompileNative("SELECT id, value, SUM(id), COUNT(*), GROUPING(id,value) AS g FROM t GROUP BY ROLLUP(id,value)", Catalog, settings);
            const auto& program = compiled.Program->Get();
            const auto& outputs = program.Results[0].Columns;
            UNIT_ASSERT(outputs[0].Type == (TType{ETypeKind::Uint64, true}));
            UNIT_ASSERT(outputs[1].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT(outputs[2].Type == (TType{ETypeKind::Uint64, true}));
            UNIT_ASSERT(outputs[3].Type == (TType{ETypeKind::Uint64}));
            UNIT_ASSERT(outputs[4].Type == (TType{ETypeKind::Uint64}));
            const TRelation* group = nullptr;
            std::optional<TExprId> mask;
            for (const auto& relation : program.Relations) {
                if (relation.Kind == ERelation::Aggregate) group = &relation;
                for (const auto& definition : relation.Definitions) if (definition.Column == outputs[4].Column) mask = definition.Expression;
            }
            UNIT_ASSERT(group && mask);
            UNIT_ASSERT_VALUES_EQUAL(group->GroupingSets.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(group->GroupingColumns.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(group->GroupingIndicators.size(), 2);
            for (const auto& [output, input] : group->GroupingColumns) UNIT_ASSERT_VALUES_UNEQUAL(output, input);
            for (std::uint64_t bits = 0; bits < 4; ++bits) {
                TEvaluationContext eval;
                eval.Column = [&](TColumnId id) {
                    for (std::size_t i = 0; i < group->GroupingIndicators.size(); ++i)
                        if (group->GroupingIndicators[i].first == id) return TValue{{ETypeKind::Uint64}, (bits >> (1 - i)) & 1};
                    throw std::logic_error("unexpected grouping mask input");
                };
                UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(Evaluate(program.Expressions, *mask, eval).Data), bits);
            }
            const auto common = CompileNative("SELECT id, value, SUM(id) FROM t GROUP BY id, ROLLUP(value)", Catalog, settings);
            UNIT_ASSERT(!common.Program->Get().Results[0].Columns[0].Type.Nullable);
            UNIT_ASSERT(!common.Program->Get().Results[0].Columns[2].Type.Nullable);
            const auto duplicate = CompileNative("SELECT id, GROUPING(id) FROM t GROUP BY GROUPING SETS ((id,id),(id),(id))", Catalog, settings);
            for (const auto& relation : duplicate.Program->Get().Relations) if (relation.Kind == ERelation::Aggregate) {
                UNIT_ASSERT_VALUES_EQUAL(relation.GroupKeys.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(relation.GroupingSets.size(), 3);
                for (const auto& set : relation.GroupingSets) UNIT_ASSERT_VALUES_EQUAL(set.size(), 1);
            }
            for (const auto* sql : {
                    "SELECT COUNT(*) FROM t GROUP BY ()",
                    "SELECT id, GROUPING(id) FROM t GROUP BY id",
                    "SELECT k, GROUPING(k) FROM t GROUP BY ROLLUP(id + 1ul AS k)",
                    "SELECT id, SUM(value) FROM t GROUP BY ROLLUP(id) HAVING GROUPING(id) = 1ul"})
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            for (const auto* sql : {
                    "SELECT GROUPING() FROM t GROUP BY id",
                    "SELECT GROUPING(value) FROM t GROUP BY id",
                    "SELECT GROUPING(SUM(id)) FROM t GROUP BY id",
                    "SELECT GROUPING(id) FROM t",
                    "SELECT id FROM t WHERE GROUPING(id) = 0ul"}) {
                try { CompileNative(sql, Catalog, settings); UNIT_FAIL(sql); }
                catch (const TUnsupportedFeature&) { UNIT_FAIL("invalid GROUPING delegated"); }
                catch (const TFrontendError&) {}
            }
            settings.Mode = ECompilerMode::Auto;
            const auto bareNull = CompileNative("SELECT k FROM t GROUP BY ROLLUP(NULL AS k)", Catalog, settings);
            UNIT_ASSERT(bareNull.UsesLegacy() && bareNull.Unsupported->Feature == EFeature::GroupingSets);
        }
    }

    Y_UNIT_TEST(OrderExpressionsUseOutputAliasesAboveProjection) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "SELECT value + 1 AS id FROM t ORDER BY id + 2",
                    "SELECT id, GROUPING(id) AS g, SUM(value) AS s FROM t GROUP BY ROLLUP(id) ORDER BY CASE WHEN g = 0ul THEN id ELSE NULL END, s + 1"}) {
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                for (const auto& relation : program.Relations) if (relation.Kind == ERelation::Map) {
                    const auto& input = program.Relations[relation.Inputs[0]].Columns;
                    for (const auto& definition : relation.Definitions) {
                        std::vector<TExprId> pending{definition.Expression};
                        while (!pending.empty()) {
                            const auto& node = program.Expressions[pending.back()]; pending.pop_back();
                            if (node.Op == EOpcode::Column)
                                UNIT_ASSERT_C(std::find(input.begin(), input.end(), node.ColumnId) != input.end(), sql);
                            pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
                        }
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(MixedDistinctAggregatesHaveAnExplicitStateBoundary) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    "SELECT COUNT(DISTINCT value), COUNT(*), SUM(value), MIN(value), MAX(value), SOME(value) FROM t",
                    "SELECT value, SUM(DISTINCT id), COUNT(DISTINCT id), COUNT(*) FROM t GROUP BY value",
                    "SELECT SUM(DISTINCT value), SUM(DISTINCT id) FROM t",
                    "SELECT AVG(DISTINCT value) FROM t", "SELECT AVG(value), AVG(id) FROM t"}) {
                UNIT_ASSERT_C(!CompileNative(sql, Catalog, settings).UsesLegacy(), sql);
            }
            for (const auto* sql : {"SELECT COUNT(DISTINCT id), AVG(value) FROM t",
                    "SELECT AVG(DISTINCT value), COUNT(*) FROM t"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported->Feature == EFeature::AggregateCombination, sql);
                auto required = settings; required.Mode = ECompilerMode::NativeRequired;
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, required), TUnsupportedFeature);
            }
        }
    }

    Y_UNIT_TEST(MixedNumericOperationsUseNativePromotion) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {"SELECT id < value, value >= id, id = value, value != id FROM t",
                    "SELECT id FROM t WHERE id BETWEEN -1 AND 100",
                    "SELECT COUNT(*) AS n FROM t HAVING COUNT(*) > 0",
                    "SELECT a.id FROM t AS a FULL JOIN t AS b ON a.id = b.value"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
            }
            for (const auto& [sql, nullable] : {std::pair{"SELECT id + value FROM t", true},
                    {"SELECT IF(true, id, value) FROM t", true}, {"SELECT COALESCE(value, id) FROM t", false}}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
                UNIT_ASSERT((result.Program->Get().Results[0].Columns[0].Type == TType{ETypeKind::Uint64, nullable}));
            }
            const auto floating = CompileNative("SELECT id + Float('1'), value + Double('1'), Float('1') + Double('2') FROM t", Catalog, settings);
            const auto& columns = floating.Program->Get().Results[0].Columns;
            UNIT_ASSERT((columns[0].Type == TType{ETypeKind::Float}));
            UNIT_ASSERT((columns[1].Type == TType{ETypeKind::Double, true}));
            UNIT_ASSERT((columns[2].Type == TType{ETypeKind::Double}));
        }
    }

    Y_UNIT_TEST(InvalidJoinConstraintsAreErrorsWithoutDelegation) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* source : {"t AS a JOIN t AS b", "t AS a LEFT JOIN t AS b", "t AS a FULL JOIN t AS b",
                    "t AS a RIGHT ONLY JOIN t AS b", "t AS a CROSS JOIN t AS b ON a.id = b.id", "t AS a, t AS b ON true"}) {
                try {
                    CompileNative(std::string("SELECT a.id FROM ") + source, Catalog, settings);
                    UNIT_FAIL("invalid JOIN was accepted");
                } catch (const TUnsupportedFeature&) {
                    UNIT_FAIL("invalid JOIN was delegated");
                } catch (const TFrontendError& error) {
                    UNIT_ASSERT_C(std::string(error.what()).find("ON or USING") != std::string::npos, error.what());
                }
            }
        }
    }

    Y_UNIT_TEST(FullJoinResidualsHaveAnExplicitBackendBoundary) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            settings.Mode = ECompilerMode::Auto;
            for (const auto* predicate : {"a.id = b.id", "a.id = b.id AND a.value = b.value", "b.value = a.value AND a.id = b.id"}) {
                const auto result = CompileNative(std::string("SELECT a.id FROM t AS a FULL JOIN t AS b ON ") + predicate, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), predicate);
            }
            for (const auto* predicate : {"a.id = b.id AND a.value > b.value", "a.id = b.id AND a.value > 0",
                    "a.id = b.id AND a.value = a.value", "a.id + 1ul = b.id", "a.id = b.id OR a.value = b.value", "true"}) {
                const auto sql = std::string("SELECT a.id FROM t AS a FULL JOIN t AS b ON ") + predicate;
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported->Feature == EFeature::Backend, predicate);
                auto required = settings; required.Mode = ECompilerMode::NativeRequired;
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, required), TUnsupportedFeature);
            }
        }
    }

    Y_UNIT_TEST(WindowsBindToSharedRelationalOperators) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            const auto plain = CompileNative("SELECT SUM(value) OVER(PARTITION BY id), AVG(value) OVER(PARTITION BY id), SUM(value) OVER(PARTITION BY id) FROM t", Catalog, settings);
            std::size_t groups = 0;
            for (const auto& relation : plain.Program->Get().Relations) {
                UNIT_ASSERT(relation.Kind != ERelation::Aggregate);
                if (relation.Kind == ERelation::Window) {
                    ++groups;
                    UNIT_ASSERT_VALUES_EQUAL(relation.WindowFunctions.size(), 2);
                    UNIT_ASSERT_VALUES_EQUAL(relation.GroupKeys.size(), 1);
                    UNIT_ASSERT(relation.WindowFrame.End == EWindowBound::UnboundedFollowing);
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(groups, 1);
            const auto& outputs = plain.Program->Get().Results[0].Columns;
            UNIT_ASSERT(outputs[0].Type == (TType{ETypeKind::Int64, true}));
            UNIT_ASSERT(outputs[1].Type == (TType{ETypeKind::Double, true}));
            const auto grouped = CompileNative("SELECT id, SUM(value), AVG(SUM(value)) OVER(), Rank() OVER(ORDER BY SUM(value) DESC) AS r FROM t GROUP BY id HAVING SUM(value) > 0 ORDER BY r", Catalog, settings);
            std::size_t aggregates = 0, windows = 0;
            for (const auto& relation : grouped.Program->Get().Relations) {
                if (relation.Kind == ERelation::Aggregate) { ++aggregates; UNIT_ASSERT_VALUES_EQUAL(relation.Aggregates.size(), 1); }
                if (relation.Kind == ERelation::Window) ++windows;
            }
            UNIT_ASSERT_VALUES_EQUAL(aggregates, 1);
            UNIT_ASSERT_VALUES_EQUAL(windows, 2);
            const auto forward = CompileNative("SELECT SUM(id) OVER(ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING), COUNT(*) OVER(), Rank() OVER(ORDER BY id DESC), DenseRank() OVER(ORDER BY id) FROM t", Catalog, settings);
            UNIT_ASSERT(forward.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Uint64, true}));
            UNIT_ASSERT(forward.Program->Get().Results[0].Columns[1].Type == (TType{ETypeKind::Uint64}));
        }
    }

    Y_UNIT_TEST(WindowDefaultsAndExplicitUnsupportedBoundaries) {
        for (const bool ansi : {false, true}) {
            TCompilerSettings settings; settings.Binding.AnsiCurrentRow = ansi;
            const auto captured = CompileNative(std::string("$q = (SELECT SUM(value) OVER(ORDER BY id) AS s FROM t); ") +
                (ansi ? "PRAGMA DisableAnsiCurrentRow; " : "PRAGMA AnsiCurrentRow; ") + "SELECT s FROM $q", Catalog, settings);
            for (const auto& relation : captured.Program->Get().Relations) if (relation.Kind == ERelation::Window)
                UNIT_ASSERT(relation.WindowFrame.Type == (ansi ? EWindowFrame::Range : EWindowFrame::Rows));
            for (const std::string pragma : {"", "PRAGMA AnsiCurrentRow; ", "PRAGMA DisableAnsiCurrentRow; "}) {
                const auto result = CompileNative(pragma + "SELECT SUM(value) OVER(ORDER BY id) FROM t", Catalog, settings);
                bool found = false;
                for (const auto& relation : result.Program->Get().Relations) if (relation.Kind == ERelation::Window) {
                    const bool expectedAnsi = pragma.empty() ? ansi : pragma == "PRAGMA AnsiCurrentRow; ";
                    UNIT_ASSERT(relation.WindowFrame.Type == (expectedAnsi ? EWindowFrame::Range : EWindowFrame::Rows));
                    found = true;
                }
                UNIT_ASSERT(found);
            }
        }
        for (const auto* sql : {
            "SELECT SUM(value) OVER w FROM t WINDOW w AS ()",
            "SELECT SUM(DISTINCT value) OVER() FROM t",
            "SELECT Lag(value) OVER(ORDER BY id) FROM t",
            "SELECT SUM(value) FILTER(WHERE value > 0) OVER() FROM t",
            "SELECT SUM(value) OVER(ORDER BY id GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t",
            "SELECT SUM(value) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING EXCLUDE CURRENT ROW) FROM t",
            "SELECT SUM(value) OVER(RANGE UNBOUNDED PRECEDING) FROM t",
            "DECLARE $n AS Int32; SELECT SUM(value) OVER(ORDER BY id ROWS $n PRECEDING) FROM t",
            "SELECT SUM(SUM(value) OVER()) OVER() FROM t",
            "SELECT id FROM t WHERE RowNumber() OVER() > 0ul",
            "SELECT SUM((SELECT 1)) OVER() FROM t",
            "SELECT id FROM t ORDER BY SUM(value) OVER()"})
            UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog), TUnsupportedFeature, sql);
        for (const auto* sql : {
            "SELECT SUM(value) OVER(ROWS BETWEEN UNBOUNDED FOLLOWING AND CURRENT ROW) FROM t",
            "SELECT SUM(value) OVER(ROWS BETWEEN CURRENT ROW AND UNBOUNDED PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ROWS -1 PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ROWS 1L PRECEDING) FROM t",
            "SELECT SUM(value) OVER(ORDER BY id, value RANGE 1 PRECEDING) FROM t"})
            UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog), TFrontendError, sql);
    }

    Y_UNIT_TEST(WindowSubqueriesRequireNoExternalCaptures) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* sql : {
                    "SELECT (SELECT SUM(value) OVER() FROM t LIMIT 1)",
                    "SELECT id FROM t WHERE value IN (SELECT s FROM (SELECT SUM(value) OVER() AS s FROM t) AS w)",
                    "SELECT EXISTS (SELECT id FROM (SELECT id, RowNumber() OVER(ORDER BY id) AS n FROM t) AS w WHERE n = 1ul)",
                    "$w = (SELECT value, RowNumber() OVER(ORDER BY id) AS n FROM t); SELECT (SELECT value FROM $w WHERE n = 1ul)",
                    "SELECT (SELECT (SELECT SUM(value) OVER() FROM t LIMIT 1))",
                    "SELECT (SELECT SUM(value) OVER() FROM t AS a LIMIT 1) FROM t AS a"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
                const auto& relations = result.Program->Get().Relations;
                UNIT_ASSERT_C(std::any_of(relations.begin(), relations.end(), [](const auto& r) { return r.Kind == ERelation::Window; }), sql);
            }
            settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {
                    "SELECT (SELECT SUM(b.value) OVER() FROM t AS b WHERE b.id = a.id LIMIT 1) FROM t AS a",
                    "SELECT EXISTS (SELECT RowNumber() OVER(PARTITION BY a.id ORDER BY b.id) FROM t AS b) FROM t AS a",
                    "SELECT a.id FROM t AS a WHERE a.value IN (SELECT s FROM (SELECT SUM(b.value) OVER() AS s FROM t AS b WHERE b.id = a.id) AS w)",
                    "SELECT (SELECT w.s FROM (SELECT SUM(value) OVER() AS s, id FROM t) AS w WHERE w.id = a.id LIMIT 1) FROM t AS a",
                    "SELECT (SELECT (SELECT SUM(b.value) OVER() FROM t AS b WHERE b.id = a.id LIMIT 1)) FROM t AS a",
                    "SELECT (SELECT SUM(value) OVER() AS v FROM t UNION ALL SELECT a.value AS v) FROM t AS a"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy(), sql);
                UNIT_ASSERT_C(result.Unsupported->Feature == EFeature::Backend, sql);
                UNIT_ASSERT_C(result.Unsupported->Message.find("window decorrelation") != std::string::npos, sql);
            }
        }
    }

    Y_UNIT_TEST(RuntimeDatesAndIntervalsUseExecutionOperations) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto result = CompileNative("DECLARE $s AS Utf8?; DECLARE $d AS Int32?; SELECT CAST($s AS Date), CAST($s AS Date) + DateTime::IntervalFromDays($d), DateTime::IntervalFromHours($d), DateTime::IntervalFromMinutes($d)", Catalog, settings);
            UNIT_ASSERT(!result.UsesLegacy());
            const auto& columns = result.Program->Get().Results[0].Columns;
            UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Date, true}));
            UNIT_ASSERT(columns[1].Type == (TType{ETypeKind::Date, true}));
            UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Interval, true}));
            UNIT_ASSERT(columns[3].Type == (TType{ETypeKind::Interval, true}));
            UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $s AS String; SELECT CAST($s AS Timestamp)", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("DECLARE $n AS Int64; SELECT DateTime::IntervalFromDays($n)", Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(CalendarBindingsAreScalarAndCannotBePersisted) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto result = CompileNative("$d = DateTime::ShiftMonths(Date('2020-01-31'), 1); $f = ($x)->{RETURN DateTime::MakeDate($x);}; SELECT $f($d), DateTime::GetYear($d), CAST(Date('2020-01-01') AS Timestamp)", Catalog, settings);
            UNIT_ASSERT(!result.UsesLegacy());
            UNIT_ASSERT(result.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Date, true}));
            UNIT_ASSERT(result.Program->Get().Results[0].Columns[1].Type == (TType{ETypeKind::Uint16, true}));
            UNIT_ASSERT(result.Program->Get().Results[0].Columns[2].Type == (TType{ETypeKind::Timestamp}));
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::Split(Date('2020-01-01'))", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::MakeDate(d) FROM (SELECT DateTime::Split(Date('2020-01-01')) AS d)", Catalog, settings), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::ShiftMonths(Date('2020-01-01'), 2147483648L)", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::GetYear(NULL)", Catalog, settings), TFrontendError);
            UNIT_ASSERT_EXCEPTION(CompileNative("SELECT DateTime::MakeDate(DateTime::ShiftMonths(NULL, 1))", Catalog, settings), TFrontendError);
        }
    }

    Y_UNIT_TEST(FastParserAndGrammarRouting) {
        for (const auto* sql : {"SELECT 1", "SELECT value FROM t WHERE value > 0", "SELECT Int32('1')", "SELECT 1 IN (1, 2,)",
                               "SELECT 'x' AS `from`", "SELECT x FROM t AS a JOIN t AS b ON a.id = b.id"}) {
            UNIT_ASSERT_C(!ParseNative(sql).UsedGeneratedParser, sql);
            AssertParserAgreement(sql);
        }
        for (const auto* sql : {"SELECT 'привет'", "SELECT @@multiline@@", "SELECT 1 NOT NULL"}) {
            UNIT_ASSERT_C(ParseNative(sql).UsedGeneratedParser, sql);
            AssertParserAgreement(sql);
        }
        for (const auto* sql : {"SELECT * AS x FROM t", "SELECT NOT NOT true", "SELECT FROM FROM t", "SELECT 1 IN ()",
                               "SELECT 'x'u", "SELECT 1 IS NULL = true", "SELECT 1 < 2 < 3", "SELECT 1 == 2 == 3",
                               "SELECT 1 UNION ALL SELECT 2 LIMIT 1", "SELECT a.x.y.* FROM a"}) AssertParserAgreement(sql);
    }

    Y_UNIT_TEST(CorrespondenceAcrossBenchmarkCorpus) {
        for (const auto& [suite, count] : {std::pair{"tpch", 22}, {"tpcds", 99}, {"clickbench", 43}}) {
            for (int i = 1; i <= count; ++i) {
                TString sql;
                UNIT_ASSERT(NResource::FindExact(TStringBuilder() << suite << "-q" << i, &sql));
                AssertParserAgreement(std::string_view(sql.data(), sql.size()));
                const auto withHelpers = TString(R"(
                    $to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };
                    $to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };
                    $round = ($x,$y) -> {return $x;};
                )") + sql;
                AssertParserAgreement(std::string_view(withHelpers.data(), withHelpers.size()));
            }
        }
    }

    Y_UNIT_TEST(PrecedenceAndSyntaxMutationCorrespondence) {
        const std::vector<std::string> atoms{"1", "true", "NULL", "a", "a.x", "'x'", "(a + 1)", "IF(true, 1, 2)", "CAST(1 AS Int64)"};
        const std::vector<std::string> ops{" + ", " * ", " || ", " ?? ", " = ", " < ", " AND ", " OR ", " IN ", " IS NULL ", " BETWEEN ", ","};
        for (const auto& a : atoms) for (const auto& b : atoms) for (const auto& op : ops) {
            AssertParserAgreement("SELECT " + a + op + b);
            AssertParserAgreement("SELECT " + a + op + b + " = true OR false");
        }
        for (const std::string sql : {"SELECT x + 1 AS y FROM t WHERE x > 2 ORDER BY y DESC LIMIT 10;",
                                     "SELECT CASE WHEN true THEN 1 ELSE 2 END, IF(false, 1);",
                                     "$f = ($x) -> { RETURN $x + 1; }; SELECT $f(1);",
                                     "WITH a AS (SELECT 1 AS x) SELECT x FROM a;"}) {
            for (std::size_t i = 0; i < sql.size(); ++i) {
                AssertParserAgreement(sql.substr(0, i) + sql.substr(i + 1));
                AssertParserAgreement(sql.substr(0, i) + "," + sql.substr(i));
            }
        }
        for (const auto* sql : {"SELECT 'a''b'", "SELECT 'a\\n'", "SELECT \"a\"\"b\"", "SELECT `a``b` FROM t",
                               "SELECT 1--comment\r,2", "SELECT 1\v", "SELECT 1; /* outer /* inner */ outer */ SELECT 2"}) {
            AssertParserAgreement(sql);
            AssertParserAgreement(sql, true);
        }
    }

    Y_UNIT_TEST(KeywordContextsFollowSharedGrammar) {
        for (const auto& keyword : Keywords) {
            const std::string word(keyword.Word);
            for (const auto& sql : {"SELECT " + word, "SELECT 1 AS " + word, "SELECT 1 " + word,
                                    "SELECT * FROM " + word, "SELECT x FROM t " + word,
                                    "SELECT " + word + "(1)", "SELECT a." + word + " FROM t AS a"}) {
                AssertParserAgreement(sql);
            }
        }
        for (const auto* sql : {"$unused = *; SELECT 1", "SELECT COUNT(DISTINCT *)", "SELECT COUNT(*,)",
                               "SELECT 1.2f", "SELECT 1foo", "SELECT 1e2u", "SELECT 0xfaceul", "SELECT 1in(1)",
                               "SELECT * + 1 FROM t", "SELECT (t.*) FROM t", "SELECT 1 WHERE *", "SELECT COUNT(ALL 1)",
                               "SELECT COUNT(ALL)", "SELECT COUNT(DISTINCT)", "SELECT t.* AS x FROM t"}) AssertParserAgreement(sql);
        for (const auto& keyword : Keywords) {
            const std::string word(keyword.Word);
            AssertParserAgreement("SELECT a.x FROM a " + word + " JOIN b ON a.x = b.x");
            AssertParserAgreement("SELECT 1 " + word + " BY x");
        }
        for (const auto* sql : {"SELECT 1 UNION ALL WITH a AS (SELECT 2) SELECT * FROM a",
                               "SELECT a.x FROM a NATURAL JOIN b ON a.x = b.x", "SELECT * FROM t OUTER JOIN s",
                               "SELECT 1 AS x UNION ALL SELECT 2 AS x", "WITH a AS (SELECT 1 AS x), SELECT x FROM a"})
            AssertParserAgreement(sql);
    }

    Y_UNIT_TEST(OnlyUnsupportedPermitsFallback) {
        TCompilerSettings settings;
        settings.Mode = ECompilerMode::Auto;
        for (const auto* sql : {"SELECT ListLength([1, 2])", "SELECT row_number() OVER ()", "SELECT * FROM a JOIN b USING(id)",
                               "PRAGMA foo='x'; SELECT 1",
                               "SELECT CASE WHEN true THEN 1 END", "SELECT * FROM a CROSS JOIN b",
                               "SELECT 'a''b'", "SELECT 1 AS 'answer'", "$'x' = 1; SELECT 1",
                               "SELECT value + 1 FROM a GROUP BY value + 1",
                               "SELECT id, STDDEV_POP(value) FROM a GROUP BY id",
                               "SELECT id, VAR_SAMP(value) FROM a GROUP BY id"}) {
            const auto result = CompileNative(sql, Catalog, settings);
            UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported, sql);
        }
        for (const auto* sql : {"SELECT FROM", "SELECT missing FROM a", "SELECT 1 + true", "DECLARE $x AS Int32; DECLARE $x AS Int32; SELECT $x"}) {
            UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TFrontendError);
        }
        settings.Parsing.MaxQueryBytes = 3;
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT 1", Catalog, settings), TResourceLimitError);
        settings.Parsing.MaxQueryBytes = 1000;
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT * FROM a", [](auto, auto) -> TCatalogTable { throw std::logic_error("catalog bug"); }, settings), std::logic_error);
    }

    Y_UNIT_TEST(RequiredAndLegacyModesAreExplicit) {
        TCompilerSettings settings;
        UNIT_ASSERT_EXCEPTION(CompileNative("SELECT ListLength([1, 2])", Catalog, settings), TUnsupportedFeature);
        settings.Mode = ECompilerMode::Legacy;
        const auto result = CompileNative("not parsed in legacy mode", [](auto, auto) -> TCatalogTable { UNIT_FAIL("unexpected catalog call"); return {}; }, settings);
        UNIT_ASSERT(result.UsesLegacy() && !result.Unsupported);
    }

    Y_UNIT_TEST(DirectivesApplyOnlyToLeadingComments) {
        UNIT_ASSERT(CompileNative("SELECT '--!syntax_pg' AS text", Catalog).Program);
        UNIT_ASSERT(CompileNative("-- ordinary comment\n--!syntax_pg\nSELECT 1", Catalog).Program);
        UNIT_ASSERT(CompileNative("--!syntax_v1\nSELECT 1", Catalog).Program);
        UNIT_ASSERT(CompileNative("--!ansi_lexer\nSELECT 'a''b'", Catalog).Program);
        UNIT_ASSERT_EXCEPTION(CompileNative("--!syntax_pg\nSELECT 1", Catalog), TUnsupportedFeature);
        TCompilerSettings settings;
        settings.Mode = ECompilerMode::Auto;
        UNIT_ASSERT_EXCEPTION(CompileNative("--!bad\nSELECT 1", Catalog, settings), TFrontendError);
    }

    Y_UNIT_TEST(CompatibleResultTypesAndParserAgreement) {
        TCompilerSettings settings;
        settings.Parser = EParser::Verify;
        auto result = CompileNative("SELECT Int32('1') AS literal, CAST(1 AS Int64) AS widened, Length('abc') AS size", Catalog, settings);
        const auto& columns = result.Program->Get().Results[0].Columns;
        UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Int32}));
        UNIT_ASSERT(columns[1].Type == (TType{ETypeKind::Int64}));
        UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Uint32}));
        auto scalar = CompileNative("SELECT 1.0 / 2.0 AS ratio, Substring('abc', 1) AS part, CAST(1 AS Double) AS converted", Catalog, settings);
        const auto& scalarColumns = scalar.Program->Get().Results[0].Columns;
        UNIT_ASSERT(scalarColumns[0].Type == (TType{ETypeKind::Double}));
        UNIT_ASSERT(scalarColumns[1].Type == (TType{ETypeKind::String}));
        UNIT_ASSERT(scalarColumns[2].Type == (TType{ETypeKind::Double}));
        auto promoted = CompileNative("SELECT 1u + 2, IF(true, 1L, 2ul)", Catalog, settings);
        UNIT_ASSERT((promoted.Program->Get().Results[0].Columns[0].Type == TType{ETypeKind::Int32}));
        UNIT_ASSERT((promoted.Program->Get().Results[0].Columns[1].Type == TType{ETypeKind::Int64}));
        for (const auto* sql : {"SELECT LOWER('ABC')",
            "SELECT CAST(Float('1') AS Int32)", "SELECT CAST(Float('1') AS Uint64)"}) {
            UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(DecimalProfileIsExplicit) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings;
            settings.Parser = parser;
            const auto result = CompileNative("SELECT Decimal('1.25',8,2), CAST(1 AS Decimal(8,2)), CAST('inf' AS Decimal(8,2)), Decimal('1',8,2) / Decimal('0',8,2)", Catalog, settings);
            const auto& columns = result.Program->Get().Results[0].Columns;
            for (std::size_t i = 0; i < columns.size(); ++i)
                UNIT_ASSERT(columns[i].Type == (TType{ETypeKind::Decimal, i == 2, 8, 2}));
            const auto common = CompileNative("DECLARE $i AS Int64; SELECT IF(true, Decimal('1.25',8,2), $i), Decimal('1',8,2) < $i", Catalog, settings);
            UNIT_ASSERT(common.Program->Get().Results[0].Columns[0].Type == (TType{ETypeKind::Decimal, false, 22, 2}));
            UNIT_ASSERT(common.Program->Get().Results[0].Columns[1].Type == TType{ETypeKind::Bool});
            const auto products = CompileNative("DECLARE $i AS Int64; DECLARE $u AS Uint64?; SELECT Decimal('1.25',3,2) * $i, $i * Decimal('1.25',3,2), Decimal('0',3,2) * $u", Catalog, settings);
            const auto& productColumns = products.Program->Get().Results[0].Columns;
            for (std::size_t i = 0; i < productColumns.size(); ++i)
                UNIT_ASSERT(productColumns[i].Type == (TType{ETypeKind::Decimal, i == 2, 3, 2}));
            const auto divisions = CompileNative("DECLARE $i AS Int64; DECLARE $u AS Uint64?; SELECT Decimal('1.25',3,2) / $i, Decimal('0',35,35) / $u, CAST(Decimal('125',8,2) / 50 AS int)", Catalog, settings);
            const auto& divisionColumns = divisions.Program->Get().Results[0].Columns;
            UNIT_ASSERT(divisionColumns[0].Type == (TType{ETypeKind::Decimal, false, 3, 2}));
            UNIT_ASSERT(divisionColumns[1].Type == (TType{ETypeKind::Decimal, true, 35, 35}));
            UNIT_ASSERT(divisionColumns[2].Type == (TType{ETypeKind::Int32, true}));
            for (const auto* sql : {"SELECT Decimal('1e2',8,2)", "SELECT CAST('1e2' AS Decimal(8,2))",
                "SELECT Decimal('1',8,2) + 1", "SELECT Decimal('1',8,2) + Decimal('1',12,2)",
                "SELECT 2 / Decimal('1',8,2)", "SELECT Decimal('1',8,2) % 2",
                "SELECT COALESCE(IF(false, Decimal('1.25',8,2)), 1L)",
                "DECLARE $s AS String; SELECT CAST($s AS Decimal(8,2))"})
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TUnsupportedFeature);
        }
    }

    Y_UNIT_TEST(SqlScalarAliasesUseCanonicalTypes) {
        using K = ETypeKind;
        for (const auto parser : {EParser::Handwritten, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto& [alias, kind] : {std::pair{"tinyint", K::Int8}, {"SMALLINT", K::Int16},
                    {"int", K::Int32}, {"Integer", K::Int32}, {"bigint", K::Int64}, {"varchar", K::String}}) {
                const auto sql = std::string("DECLARE $x AS ") + alias + "?; SELECT $x, CAST('12' AS " + alias + "), " + alias + "('12')";
                const auto compiled = CompileNative(sql, Catalog, settings);
                const auto& program = compiled.Program->Get();
                UNIT_ASSERT(program.Parameters.at("x") == (TType{kind, true}));
                const auto& columns = program.Results[0].Columns;
                UNIT_ASSERT(columns[0].Type == (TType{kind, true}));
                UNIT_ASSERT(columns[1].Type == (TType{kind, kind != K::String}));
                UNIT_ASSERT(columns[2].Type == (TType{kind}));
            }
            for (const auto* sql : {"SELECT CAST(1 AS byte)", "SELECT CAST('a' AS varchar123)", "SELECT CAST('a' AS varchar(12))"})
                UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
        }
    }

    Y_UNIT_TEST(DiscoverOnlyReachableTablesWithBindingPrefixAndCteScope) {
        const auto syntax = ParseNative("$unused = SELECT * FROM unused; $q = SELECT * FROM a; PRAGMA TablePathPrefix='/Root'; WITH c AS (SELECT * FROM b) SELECT * FROM $q CROSS JOIN c; SELECT * FROM a");
        const auto tables = CollectTables(syntax);
        UNIT_ASSERT_VALUES_EQUAL(tables.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(tables[0].Name, "/Root/a");
        UNIT_ASSERT_VALUES_EQUAL(tables[1].Name, "/Root/b");
    }
}
