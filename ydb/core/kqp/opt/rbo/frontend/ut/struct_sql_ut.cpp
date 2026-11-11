#include <ydb/core/kqp/opt/rbo/frontend/bindings.h>
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
TBoundProgram Compile(std::string_view sql, EParser parser,
    std::shared_ptr<const TExternalBindings> bindings = {}) {
    TCompilerSettings settings;
    settings.Parser = parser;
    settings.Binding.ExternalBindings = std::move(bindings);
    auto result = CompileNative(sql, {}, settings);
    UNIT_ASSERT(result.Program);
    return std::move(*result.Program).Release();
}

TValue ProjectedValue(const TBoundProgram& program, std::size_t index) {
    TEvaluationContext context;
    context.Column = [&](TColumnId column) -> TValue {
        for (const auto& relation : program.Relations) for (const auto& definition : relation.Definitions)
            if (definition.Column == column) return Evaluate(program.Expressions, definition.Expression, context);
        throw std::logic_error("missing test column definition");
    };
    return context.Column(program.Results.at(0).Columns.at(index).Column);
}
}

Y_UNIT_TEST_SUITE(KqpNativeStructSql) {
    Y_UNIT_TEST(BothParsersRetainNamesAndMemberBoundaries) {
        for (const auto* sql : {
                "SELECT AsStruct(1 AS k, 'x' AS text).k;",
                "SELECT (AsStruct(2 AS `a.b`)).`a.b`;",
                "SELECT [AsStruct(3 AS k)][0u].k;",
                "SELECT $record.`some``field`;",
                "SELECT a.b.c, `a.b`.c, a.`b.c`, `a.b.c` FROM t;",
                "SELECT AsStruct(AsStruct(4 AS k) AS nested).nested.k;"}) {
            UNIT_ASSERT_VALUES_EQUAL_C(SyntaxFingerprint(Parse(sql)), SyntaxFingerprint(ParseGrammar(sql)), sql);
        }
        UNIT_ASSERT_VALUES_UNEQUAL(SyntaxFingerprint(Parse("SELECT a.`b.c`;")),
            SyntaxFingerprint(Parse("SELECT a.b.c;")));
        UNIT_ASSERT_VALUES_UNEQUAL(SyntaxFingerprint(Parse("SELECT `a.b`;")),
            SyntaxFingerprint(Parse("SELECT a.b;")));
    }

    // PR58587's collection/access contract, including its externally supplied
    // record and escaped field. The native boundary returns typed expressions.
    Y_UNIT_TEST(CollectionsAndAccessAreRewrittenNatively) {
        auto arena = std::make_shared<TExpressionArena>();
        const std::pair<std::string, TExprId> fields[]{{"some`field", arena->Literal({ETypeKind::Int32}, std::int64_t{42})}};
        auto bindings = std::make_shared<TExternalBindings>();
        bindings->Values.emplace("record", TScalarBinding(arena, arena->Struct(fields)));
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            const auto record = Compile("SELECT $record.`some``field`;", parser, bindings);
            UNIT_ASSERT(record.Results[0].Columns[0].Type == TType{ETypeKind::Int32});
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(ProjectedValue(record, 0)), "42");
            const auto tuple = Compile("SELECT (1, 'a'), AsTuple(1, 'a'), [1, 2l], [1, 2][0u];", parser);
            const auto& columns = tuple.Results[0].Columns;
            UNIT_ASSERT(columns[0].Type == columns[1].Type);
            UNIT_ASSERT(columns[0].Type.Kind == ETypeKind::Tuple);
            const auto items = tuple.Expressions.TupleItems(columns[0].Type);
            UNIT_ASSERT_VALUES_EQUAL(items.size(), 2);
            UNIT_ASSERT(items[0] == TType{ETypeKind::Int32});
            UNIT_ASSERT(items[1] == TType{ETypeKind::String});
            UNIT_ASSERT(tuple.Expressions.ListItem(columns[2].Type) == TType{ETypeKind::Int64});
            UNIT_ASSERT(columns[3].Type == (TType{ETypeKind::Int32, true}));
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(ProjectedValue(tuple, 3)), "1");
        }
    }

    Y_UNIT_TEST(ComputedAndQualifiedMembersUseTheSameTypesAndValues) {
        const std::pair<const char*, const char*> cases[]{
            {"SELECT AsStruct(1 AS k).k;", "1"},
            {"SELECT AsStruct(AsStruct(2 AS `b.c`) AS a).a.`b.c`;", "2"},
            {"SELECT [AsStruct(3 AS k)][0u].k;", "3"},
            {"SELECT (AsStruct(4 AS k)).k;", "4"},
            {"SELECT r.k FROM (SELECT AsStruct(7 AS k) AS r) AS s;", "7"},
            {"SELECT s.r.k FROM (SELECT AsStruct(7 AS k) AS r) AS s;", "7"},
            {"SELECT `a.b`.`r.s`.`k.l` FROM (SELECT AsStruct(9 AS `k.l`) AS `r.s`) AS `a.b`;", "9"},
            {"SELECT (Just(AsStruct(5 AS k))).k;", "5"},
        };
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) for (const auto& [sql, expected] : cases) {
            const auto program = Compile(sql, parser);
            UNIT_ASSERT_VALUES_EQUAL_C(FormatValue(ProjectedValue(program, 0)), expected, sql);
            UNIT_ASSERT_VALUES_EQUAL_C(program.Results[0].Columns[0].Name, "column0", sql);
        }
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            const auto program = Compile("SELECT `r.k`, r.k FROM (SELECT 11 AS `r.k`, AsStruct(12 AS k) AS r) AS s;", parser);
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(ProjectedValue(program, 0)), "11");
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(ProjectedValue(program, 1)), "12");
            UNIT_ASSERT_VALUES_EQUAL(program.Results[0].Columns[0].Name, "r.k");
            UNIT_ASSERT_EXCEPTION_CONTAINS(
                Compile("SELECT s.id FROM (SELECT 8 AS `s.id`, 7 AS id) AS s;", parser),
                TFrontendError, "ambiguous column");
        }
    }

    Y_UNIT_TEST(GroupingAliasesAndCorrelationsResolveTheBaseColumn) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            for (const auto* sql : {
                    "SELECT s.r.k, SUM(1) FROM (SELECT AsStruct(7 AS k) AS r) AS s GROUP BY r.k;",
                    "SELECT AsStruct(7 AS k) AS r ORDER BY r.k;",
                    "SELECT (SELECT s.r.k) FROM (SELECT AsStruct(7 AS k) AS r) AS s;"}) {
                const auto program = Compile(sql, parser);
                UNIT_ASSERT_C(!program.Results.empty(), sql);
            }
            for (const auto* sql : {
                    "SELECT s.r.k, SUM(1) FROM (SELECT AsStruct(7 AS k) AS r) AS s;",
                    "SELECT (SELECT s.r.k FROM (SELECT 1 AS other) AS s) FROM (SELECT AsStruct(7 AS k) AS r) AS s;",
                    "SELECT (SELECT r.k FROM (SELECT 1 AS r) AS local) FROM (SELECT AsStruct(7 AS k) AS r) AS outer_row;",
                    "SELECT r.k FROM (SELECT AsStruct(1 AS k) AS r) AS a CROSS JOIN (SELECT AsStruct(2 AS k) AS r) AS b;"}) {
                UNIT_ASSERT_EXCEPTION_C(Compile(sql, parser), TFrontendError, sql);
            }
        }
    }

    Y_UNIT_TEST(ReservedColumnNamesDoNotRestrictStructFields) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            for (const auto* sql : {
                    "SELECT AsStruct(7 AS _yql_field)._yql_field;",
                    "SELECT s.r._yql_field FROM (SELECT AsStruct(7 AS _yql_field) AS r) AS s;",
                    "SELECT r._yql_field FROM (SELECT AsStruct(7 AS _yql_field) AS r) AS s;"}) {
                const auto program = Compile(sql, parser);
                UNIT_ASSERT_VALUES_EQUAL_C(FormatValue(ProjectedValue(program, 0)), "7", sql);
            }
            for (const auto* sql : {
                    "SELECT _yql_field;",
                    "SELECT s._yql_field FROM (SELECT 7 AS _yql_field) AS s;",
                    "SELECT _yql_field FROM (SELECT 7 AS _yql_field) AS s;"})
                UNIT_ASSERT_EXCEPTION_CONTAINS(Compile(sql, parser), TUnsupportedFeature, "system columns");
        }
    }

    Y_UNIT_TEST(DiscardedFieldsDoNotRunCheckedErrors) {
        for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
            const auto program = Compile("SELECT AsStruct(7 AS k, Ensure(8, FALSE, 'discarded') AS bad).k;", parser);
            UNIT_ASSERT_VALUES_EQUAL(FormatValue(ProjectedValue(program, 0)), "7");
            const auto failed = Compile("SELECT AsStruct(7 AS k, Ensure(8, FALSE, 'selected') AS bad).bad;", parser);
            UNIT_ASSERT_EXCEPTION(ProjectedValue(failed, 0), TFrontendError);
            for (const auto* sql : {"SELECT AsStruct(1);", "SELECT AsStruct(1 AS k, 2 AS k);",
                    "SELECT AsStruct(1 AS k).missing;", "SELECT AsTuple(1 AS k);", "SELECT (1).k;"})
                UNIT_ASSERT_EXCEPTION_C(Compile(sql, parser), TFrontendError, sql);
        }
        const TCatalogColumn columns[]{{"id", {ETypeKind::Int64}}};
        const auto row = CompileRowExpression("AsStruct(id AS k, Ensure(id, FALSE, 'discarded') AS bad).k", columns, {ETypeKind::Int64});
        UNIT_ASSERT_VALUES_EQUAL(row.Dependencies.size(), 1);
        TEvaluationContext context;
        context.Column = [](auto) { return TValue{{ETypeKind::Int64}, std::int64_t{19}}; };
        UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(row.Expressions, row.Root, context)), "19");
    }
}
