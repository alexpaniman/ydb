#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/parser.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/resource/resource.h>
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

    Y_UNIT_TEST(MixedIntegerComparisonsDoNotEnableMixedArithmetic) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            TCompilerSettings settings; settings.Parser = parser; settings.Mode = ECompilerMode::Auto;
            for (const auto* sql : {"SELECT id < value, value >= id, id = value, value != id FROM t",
                    "SELECT id FROM t WHERE id BETWEEN -1 AND 100",
                    "SELECT COUNT(*) AS n FROM t HAVING COUNT(*) > 0",
                    "SELECT a.id FROM t AS a FULL JOIN t AS b ON a.id = b.value"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(!result.UsesLegacy(), sql);
            }
            for (const auto* sql : {"SELECT id + value FROM t", "SELECT IF(true, id, value) FROM t", "SELECT COALESCE(value, id) FROM t"}) {
                const auto result = CompileNative(sql, Catalog, settings);
                UNIT_ASSERT_C(result.UsesLegacy() && result.Unsupported->Feature == EFeature::Type, sql);
            }
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
            "SELECT (SELECT SUM(value) OVER() FROM t LIMIT 1)",
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
                               "SELECT id, STDDEV_SAMPLE(value) FROM a GROUP BY id",
                               "SELECT id, STDDEV_SAMP(value) FROM a GROUP BY id"}) {
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
        for (const auto* sql : {"SELECT 1u + 2", "SELECT IF(true, 1L, 2ul)", "SELECT LOWER('ABC')",
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
            for (const auto* sql : {"SELECT Decimal('1e2',8,2)", "SELECT CAST('1e2' AS Decimal(8,2))",
                "SELECT Decimal('1',8,2) + 1", "SELECT Decimal('1',8,2) + Decimal('1',12,2)",
                "SELECT Decimal('1',8,2) / 2", "SELECT Decimal('1',8,2) % 2",
                "SELECT COALESCE(IF(false, Decimal('1.25',8,2)), 1L)",
                "DECLARE $s AS String; SELECT CAST($s AS Decimal(8,2))"})
                UNIT_ASSERT_EXCEPTION(CompileNative(sql, Catalog, settings), TUnsupportedFeature);
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
