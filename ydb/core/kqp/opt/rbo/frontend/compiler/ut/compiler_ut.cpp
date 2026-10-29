#include <ydb/core/kqp/opt/rbo/frontend/compiler/compiler.h>
#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TCatalogTable Catalog(std::string_view name, TSourcePosition) {
    return {std::string(name), {{"id", {ETypeKind::Uint64}}, {"value", {ETypeKind::Int32, true}}}};
}
}

Y_UNIT_TEST_SUITE(KqpNativeCompiler) {
    Y_UNIT_TEST(OnlyUnsupportedPermitsFallback) {
        TCompilerSettings settings;
        settings.Mode = ECompilerMode::Auto;
        for (const auto* sql : {"SELECT ListLength([1, 2])", "SELECT row_number() OVER ()", "SELECT * FROM a JOIN b USING(id)",
                               "SELECT value IN (SELECT value FROM b) FROM a", "PRAGMA foo='x'; SELECT 1",
                               "SELECT CASE WHEN true THEN 1 END", "SELECT * FROM a CROSS JOIN b",
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
        const auto& columns = result.Program->Results[0].Columns;
        UNIT_ASSERT(columns[0].Type == (TType{ETypeKind::Int32}));
        UNIT_ASSERT(columns[1].Type == (TType{ETypeKind::Int64}));
        UNIT_ASSERT(columns[2].Type == (TType{ETypeKind::Uint32}));
        for (const auto* sql : {"SELECT 1u + 2", "SELECT 1.0 / 2.0", "SELECT Substring('abc', 1)", "SELECT CAST('1' AS Decimal(8,2))", "SELECT LOWER('ABC')"}) {
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
