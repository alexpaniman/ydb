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
                               "SELECT value IN (SELECT value FROM b) FROM a", "PRAGMA foo='x'; SELECT 1",
                               "SELECT CASE WHEN true THEN 1 END", "SELECT * FROM a CROSS JOIN b",
                               "SELECT 'a''b'", "SELECT 1 AS 'answer'", "$'x' = 1; SELECT 1",
                               "SELECT value + 1 FROM a GROUP BY value + 1"}) {
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
        for (const auto* sql : {"SELECT 1u + 2", "SELECT CAST('1' AS Decimal(8,2))", "SELECT LOWER('ABC')",
            "SELECT CAST(Float('1') AS Int32)", "SELECT CAST(Float('1') AS Uint64)"}) {
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
