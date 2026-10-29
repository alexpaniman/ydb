#include <ydb/core/kqp/opt/rbo/frontend/binder.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TCatalogTable Catalog(std::string_view name, TSourcePosition pos) {
    if (name != "a" && name != "b" && name != "/Root/a") throw TFrontendError(pos, "unknown table");
    return {std::string(name), {{"id", {ETypeKind::Uint64}}, {"value", {ETypeKind::Int32, true}}, {"name", {ETypeKind::String}}}};
}

const TRelation& Find(const TBoundProgram& program, ERelation kind) {
    for (const auto& relation : program.Relations) if (relation.Kind == kind) return relation;
    UNIT_FAIL("operator not found");
    throw std::runtime_error("operator not found");
}
}

Y_UNIT_TEST_SUITE(KqpNativeBinder) {
    Y_UNIT_TEST(DirectReadFilterProjection) {
        const auto program = Compile("SELECT id, value + 1 AS next FROM a WHERE value > 0 ORDER BY next DESC LIMIT 10", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(program.Results.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns[1].Name, "next");
        UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int32, true}));
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::Read).Table, "a");
        UNIT_ASSERT(Find(program, ERelation::Sort).SortKeys[0].Descending);
        UNIT_ASSERT(Find(program, ERelation::Filter).Predicate.has_value());
        for (std::size_t i = 0; i < program.Expressions.Size(); ++i) {
            for (auto child : program.Expressions[static_cast<TExprId>(i)].Arguments) UNIT_ASSERT(child < i);
        }
    }

    Y_UNIT_TEST(AggregatesAreRelationalAndTypedOnce) {
        const auto program = Compile("SELECT name, SUM(value) + 1 AS total, COUNT(*) AS n FROM a GROUP BY name HAVING SUM(value) > 3 ORDER BY total", Catalog);
        const auto& aggregate = Find(program, ERelation::Aggregate);
        UNIT_ASSERT_VALUES_EQUAL(aggregate.GroupKeys.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(aggregate.Aggregates.size(), 2);
        UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int64, true}));
        UNIT_ASSERT(program.Results[0].Columns[2].Type == (TType{ETypeKind::Uint64}));
    }

    Y_UNIT_TEST(GroupingExpressionsAndAliases) {
        const auto program = Compile("SELECT k, COUNT(*) FROM a GROUP BY value + 1 AS k", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::Aggregate).GroupKeys.size(), 1);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT id, SUM(value) FROM a", Catalog), TFrontendError);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT SUM(MAX(value)) FROM a", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(JoinScopeAndOuterNullability) {
        const auto program = Compile("SELECT a.id AS a_id, b.id AS b_id FROM a LEFT JOIN b ON a.id == b.id", Catalog);
        UNIT_ASSERT(!program.Results[0].Columns[0].Type.Nullable);
        UNIT_ASSERT(program.Results[0].Columns[1].Type.Nullable);
        UNIT_ASSERT_VALUES_UNEQUAL(program.Results[0].Columns[0].Column, program.Results[0].Columns[1].Column);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT id FROM a JOIN b ON a.id == b.id", Catalog), TFrontendError);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT b.id FROM a LEFT SEMI JOIN b ON a.id == b.id", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(ScalarLambdaSpecializationAndParameters) {
        const auto program = Compile(R"(
            DECLARE $p AS Int32;
            $twice = ($x) -> { RETURN $x * 2; };
            SELECT $twice($p) AS x, $twice(1L) AS y;
        )", Catalog);
        const auto& definitions = Find(program, ERelation::Map).Definitions;
        TEvaluationContext ctx;
        ctx.Parameter = [](std::string_view name) { UNIT_ASSERT_VALUES_EQUAL(name, "p"); return TValue{{ETypeKind::Int32}, std::int64_t{3}}; };
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(program.Expressions, definitions[0].Expression, ctx).Data), 6);
        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int32}));
        UNIT_ASSERT(program.Results[0].Columns[1].Type == (TType{ETypeKind::Int64}));
    }

    Y_UNIT_TEST(NamedSourcesGetDistinctBindings) {
        const auto program = Compile("$q = SELECT id FROM a; SELECT l.id, r.id FROM $q AS l JOIN $q AS r ON l.id == r.id", Catalog);
        UNIT_ASSERT_VALUES_UNEQUAL(program.Results[0].Columns[0].Column, program.Results[0].Columns[1].Column);
        UNIT_ASSERT_EXCEPTION(Compile("$q = SELECT * FROM $q; SELECT * FROM $q", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(UnionAlignsTypesAndPreservesNames) {
        const auto program = Compile("SELECT 1 AS first UNION ALL SELECT 2L AS second", Catalog);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns[0].Name, "first");
        UNIT_ASSERT(program.Results[0].Columns[0].Type == (TType{ETypeKind::Int64}));
        UNIT_ASSERT_VALUES_EQUAL(Find(program, ERelation::UnionAll).Inputs.size(), 2);
        UNIT_ASSERT_EXCEPTION(Compile("SELECT 1 UNION ALL SELECT 1, 2", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(UnknownFeaturesAreErrors) {
        for (auto text : {"PRAGMA unknown; SELECT 1", "SELECT UnknownFunction(1)", "SELECT missing FROM a", "SELECT $undeclared", "SELECT 1 LIMIT -1", "SELECT id FROM a WHERE SUM(value) > 0"}) {
            UNIT_ASSERT_EXCEPTION(Compile(text, Catalog), TFrontendError);
        }
    }

    Y_UNIT_TEST(NullInAndLazyCaseAreExecutable) {
        const auto program = Compile("SELECT 2 IN (1, NULL) AS x, CASE WHEN true THEN 7 ELSE 1/0 END AS y", Catalog);
        const auto& definitions = Find(program, ERelation::Map).Definitions;
        UNIT_ASSERT(Evaluate(program.Expressions, definitions[0].Expression).IsNull());
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::int64_t>(Evaluate(program.Expressions, definitions[1].Expression).Data), 7);
    }

    Y_UNIT_TEST(RepeatedProjectionHasDistinctBindings) {
        const auto program = Compile("SELECT id AS a, id AS b FROM a", Catalog);
        UNIT_ASSERT_VALUES_UNEQUAL(program.Results[0].Columns[0].Column, program.Results[0].Columns[1].Column);
    }

    Y_UNIT_TEST(SimpleLikePatterns) {
        const auto program = Compile("SELECT 'abc' LIKE '%b%', 'abc' LIKE 'ab%', 'abc' NOT LIKE '%d'", Catalog);
        for (const auto& definition : Find(program, ERelation::Map).Definitions) {
            UNIT_ASSERT(std::get<bool>(Evaluate(program.Expressions, definition.Expression).Data));
        }
        UNIT_ASSERT_EXCEPTION(Compile("SELECT 'abc' LIKE 'a_c'", Catalog), TFrontendError);
    }

    Y_UNIT_TEST(LinearProjectionGrowth) {
        std::string sql = "SELECT ";
        for (unsigned i = 0; i < 2000; ++i) sql += (i ? "," : "") + std::string("value + ") + std::to_string(i) + " AS c" + std::to_string(i);
        sql += " FROM a";
        const auto program = Compile(sql, Catalog);
        UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns.size(), 2000);
        UNIT_ASSERT(program.Expressions.Size() < 12000);
    }
}
