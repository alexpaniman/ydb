#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <library/cpp/testing/unittest/registar.h>

#include <algorithm>
#include <unordered_set>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TCatalogTable Catalog(std::string_view name, TSourcePosition) {
    const bool left = name == "a";
    return {std::string(name), {{"k", {left ? ETypeKind::Int32 : ETypeKind::Int64}},
        {left ? "lv" : "rv", {ETypeKind::String, true}}}};
}
}

Y_UNIT_TEST_SUITE(KqpNativeJoinUsing) {
    Y_UNIT_TEST(KeysHaveDistinctQualifiedAndMergedBindings) {
        for (const auto parser : {EParser::Fast, EParser::Grammar, EParser::Verify}) {
            TCompilerSettings settings; settings.Parser = parser;
            for (const auto* join : {"INNER", "LEFT", "RIGHT", "FULL", "EXCLUSION"}) {
                const auto compiled = CompileNative(std::string("SELECT k, l.k AS lk, r.k AS rk FROM a AS l ") +
                    join + " JOIN b AS r USING(k)", Catalog, settings);
                const auto& p = compiled.Program->Get();
                const auto& cols = p.Results[0].Columns;
                UNIT_ASSERT_VALUES_EQUAL(cols.size(), 3);
                UNIT_ASSERT(cols[0].Column != cols[1].Column && cols[0].Column != cols[2].Column);
                UNIT_ASSERT(cols[1].Column != cols[2].Column);
                const std::string_view kind(join);
                const bool padLeft = kind == "RIGHT" || kind == "FULL" || kind == "EXCLUSION";
                const bool padRight = kind == "LEFT" || kind == "FULL" || kind == "EXCLUSION";
                UNIT_ASSERT(cols[0].Type == (TType{ETypeKind::Int64, padLeft && padRight}));
                UNIT_ASSERT(cols[1].Type == (TType{ETypeKind::Int32, padLeft}));
                UNIT_ASSERT(cols[2].Type == (TType{ETypeKind::Int64, padRight}));
                const auto relation = std::find_if(p.Relations.begin(), p.Relations.end(),
                    [](const auto& r) { return r.Kind == ERelation::Join; });
                UNIT_ASSERT(relation != p.Relations.end());
                UNIT_ASSERT(relation->Predicate);
                UNIT_ASSERT(p.Expressions[*relation->Predicate].Op == EOpcode::Equal);
            }
        }
    }

    Y_UNIT_TEST(StarsRespectKeyVisibilityAndSemiJoinInputs) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* projection : {"*", "l.*", "r.*"}) {
            const auto compiled = CompileNative(std::string("SELECT ") + projection +
                " FROM a AS l FULL JOIN b AS r USING(k)", Catalog, settings);
            std::unordered_set<std::string> names;
            for (const auto& column : compiled.Program->Get().Results[0].Columns) UNIT_ASSERT(names.insert(column.Name).second);
            UNIT_ASSERT(names.contains("k"));
            UNIT_ASSERT_VALUES_EQUAL(names.size(), std::string_view(projection) == "*" ? 3 : 2);
        }
        for (const auto* join : {"LEFT SEMI", "LEFT ONLY", "RIGHT SEMI", "RIGHT ONLY"}) {
            const auto compiled = CompileNative(std::string("SELECT * FROM a AS l ") + join + " JOIN b AS r USING(k)", Catalog, settings);
            const auto& columns = compiled.Program->Get().Results[0].Columns;
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(columns[0].Name, "k");
            UNIT_ASSERT(columns[0].Type == (TType{std::string_view(join).starts_with("LEFT") ? ETypeKind::Int32 : ETypeKind::Int64}));
        }
    }

    Y_UNIT_TEST(MergedKeysRemainAvailableToLaterClausesAndSources) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "SELECT k, COUNT(*) FROM a AS l FULL JOIN b AS r USING(k) WHERE k > 0 GROUP BY k ORDER BY k",
            "SELECT k FROM a AS l FULL JOIN b AS r USING(k) JOIN a AS x USING(k)",
            "SELECT d.k FROM (SELECT * FROM a AS l FULL JOIN b AS r USING(k)) AS d",
            "SELECT k FROM a AS l FULL JOIN b AS r USING(k) WHERE EXISTS (SELECT 1 FROM a AS x WHERE x.k = k)",
        }) UNIT_ASSERT_C(CompileNative(sql, Catalog, settings).Program, sql);
    }

    Y_UNIT_TEST(InvalidKeysAreDiagnosedDuringBinding) {
        TCompilerSettings settings; settings.Parser = EParser::Verify;
        for (const auto* sql : {
            "SELECT k FROM a AS l JOIN b AS r USING(missing)",
            "SELECT k FROM a AS l JOIN b AS r USING(lv)",
            "SELECT k FROM a AS l JOIN b AS r USING(k, k)",
            "SELECT k FROM a AS l CROSS JOIN b AS r USING(k)",
            "SELECT k FROM a AS l JOIN b AS r ON l.k = r.k JOIN a AS x USING(k)",
        }) UNIT_ASSERT_EXCEPTION_C(CompileNative(sql, Catalog, settings), TFrontendError, sql);
    }
}
