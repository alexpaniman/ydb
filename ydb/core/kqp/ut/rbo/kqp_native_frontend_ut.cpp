#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_compatibility.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>
#include <yql/essentials/minikql/comp_nodes/ut/mkql_computation_node_ut.h>
#include <yql/essentials/minikql/mkql_type_ops.h>
#include <yql/essentials/providers/common/mkql/yql_provider_mkql.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

#include <cmath>

namespace NKikimr::NKqp {
namespace {
TNativeTable NoTables(std::string_view, NFrontend::TSourcePosition pos) {
    throw NFrontend::TFrontendError(pos, "unexpected table lookup");
}

NFrontend::TExprId Projection(const NFrontend::TBoundProgram& program) {
    const auto column = program.Results.at(0).Columns.at(0).Column;
    for (const auto& relation : program.Relations) {
        for (const auto& definition : relation.Definitions) if (definition.Column == column) return definition.Expression;
    }
    ythrow yexception() << "projection not found";
}

// Compile and execute the terminal scalar graph directly: no YQL annotation,
// expression peepholes, SQL translation or compatibility transformer.
void CheckScalar(std::string_view sql) {
    auto program = NFrontend::NDetail::Compile(sql, [](auto, auto pos) -> NFrontend::TCatalogTable {
        throw NFrontend::TFrontendError(pos, "unexpected table");
    });
    const auto id = Projection(program);
    const auto expected = NFrontend::Evaluate(program.Expressions, id);
    TExprContext ctx;
    const auto lambda = LowerNativeLambda(program.Expressions, id, ctx, {});
    UNIT_ASSERT_C(AreAllNodesTypeAnnotated(*lambda), sql);
    UNIT_ASSERT_C(!NeedsRboCompatibilityLowering(lambda), sql);
    NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
    auto registry = setup.FunctionRegistry->Clone();
    NMiniKQL::FillStaticModules(*registry);
    setup.FunctionRegistry = registry;
    setup.PgmBuilder.Reset(new NMiniKQL::TProgramBuilder(*setup.Env, *registry));
    NYql::NCommon::TMkqlCommonCallableCompiler compiler;
    NYql::NCommon::TMkqlBuildContext build(compiler, *setup.PgmBuilder, ctx);
    const auto runtime = NYql::NCommon::MkqlBuildLambda(*lambda, build, {setup.PgmBuilder->NewEmptyStruct()});
    auto graph = setup.BuildGraph(runtime);
    const auto value = graph->GetValue();
    if (expected.IsNull()) {
        UNIT_ASSERT_C(!value, sql);
        return;
    }
    UNIT_ASSERT_C(value, sql);
    using K = NFrontend::ETypeKind;
    switch (expected.Type.Kind) {
        case K::Bool: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<bool>(), std::get<bool>(expected.Data), sql); break;
        case K::Int8: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i8>(), std::get<std::int64_t>(expected.Data), sql); break;
        case K::Int16: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i16>(), std::get<std::int64_t>(expected.Data), sql); break;
        case K::Int32: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i32>(), std::get<std::int64_t>(expected.Data), sql); break;
        case K::Int64: case K::Interval: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i64>(), std::get<std::int64_t>(expected.Data), sql); break;
        case K::Uint8: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui8>(), std::get<std::uint64_t>(expected.Data), sql); break;
        case K::Uint16: case K::Date: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui16>(), std::get<std::uint64_t>(expected.Data), sql); break;
        case K::Uint32: case K::Datetime: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui32>(), std::get<std::uint64_t>(expected.Data), sql); break;
        case K::Uint64: case K::Timestamp: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui64>(), std::get<std::uint64_t>(expected.Data), sql); break;
        case K::Float: case K::Double: {
            const auto actual = expected.Type.Kind == K::Float ? double(value.Get<float>()) : value.Get<double>();
            const auto number = std::get<double>(expected.Data);
            UNIT_ASSERT_C((std::isnan(actual) && std::isnan(number)) || actual == number, sql);
            break;
        }
        case K::String: case K::Utf8: UNIT_ASSERT_VALUES_EQUAL_C(TString(value.AsStringRef()), TString(std::get<std::string>(expected.Data)), sql); break;
        case K::Decimal: UNIT_ASSERT_C(value.GetInt128() == std::get<NFrontend::TDecimal>(expected.Data).Coefficient, sql); break;
        case K::Null: UNIT_FAIL("unexpected non-null Null");
    }
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeFrontend) {
    Y_UNIT_TEST(TextCastsMatchRuntimeLexicalContract) {
        using namespace NFrontend;
        for (const auto kind : {ETypeKind::Bool, ETypeKind::Float, ETypeKind::Double}) {
            const auto slot = kind == ETypeKind::Bool ? NUdf::EDataSlot::Bool :
                kind == ETypeKind::Float ? NUdf::EDataSlot::Float : NUdf::EDataSlot::Double;
            for (const std::string text : {"", " ", " 1", "1 ", "\t1", "1\n", "0", "-0", "+0", "1", "-1", "+1",
                ".5", "1.", "1e2", "-1.25E-2", "1e9999", "-1e9999", "1e-9999", "-1e-9999",
                "0x10", "-0x10", "0x1p2", "nan", "+NaN", "-NaN", "NaN(1)", "inf", "+Inf", "-Inf",
                "infinity", " inf", " true", "true", "TRUE", "false", "FALSE", "yes", "no", "bad", "1junk",
                "3.4028234663852886e38", "1.7976931348623157e308", "2.2250738585072014e-308", "4.9406564584124654e-324"}) {
                TExpressionArena arena;
                const auto input = arena.Literal({ETypeKind::String}, text);
                const auto result = Evaluate(arena, arena.Cast(input, {kind}));
                const auto expected = NMiniKQL::ValueFromString(slot, NUdf::TStringRef(text.data(), text.size()));
                const auto context = TStringBuilder() << TType{kind}.ToString() << ": '" << text << "'";
                UNIT_ASSERT_VALUES_EQUAL_C(result.IsNull(), !expected, context);
                if (!expected) continue;
                if (kind == ETypeKind::Bool) {
                    UNIT_ASSERT_VALUES_EQUAL_C(std::get<bool>(result.Data), expected.Get<bool>(), context);
                } else {
                    const auto value = std::get<double>(result.Data);
                    const auto number = kind == ETypeKind::Float ? double(expected.Get<float>()) : expected.Get<double>();
                    UNIT_ASSERT_C((std::isnan(value) && std::isnan(number)) || value == number, context);
                    if (value == 0) UNIT_ASSERT_VALUES_EQUAL_C(std::signbit(value), std::signbit(number), context);
                }
            }
        }
    }

    Y_UNIT_TEST(DirectRboAndNoYqlAnnotation) {
        NTests::TIdTestContext f;
        auto roots = CompileNativeToRbo(R"(
            SELECT a.id, b.id AS right_id, a.id + 2 AS next
            FROM (SELECT 1 AS id) AS a
            LEFT JOIN (SELECT CAST(1L AS Int32) AS id) AS b ON a.id == b.id
            WHERE a.id > 0 ORDER BY next LIMIT 5
        )", NoTables, f.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(roots.size(), 1);
        auto& root = *roots.front();
        UNIT_ASSERT(root.PlanProps.NativeExpressions);
        bool joinFound = false;
        for (const auto& item : root) {
            for (const auto& expression : item.Current->GetExpressions()) {
                UNIT_ASSERT(expression.get().IsNative());
                UNIT_ASSERT(!expression.get().Node);
            }
            if (item.Current->Kind == EOperator::Join) {
                const auto& join = CastOperator<TOpJoin>(*item.Current);
                UNIT_ASSERT_VALUES_EQUAL(join.JoinKeys.Items().size(), 1);
                UNIT_ASSERT(join.JoinFilters.empty());
                joinFound = true;
            }
        }
        UNIT_ASSERT(joinFound);
        auto forbidden = CreateFunctorTransformer([](TExprNode::TPtr, TExprNode::TPtr&, TExprContext&) {
            UNIT_FAIL("native expression invoked YQL type annotation");
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext nativeContext(f.KqpCtx, f.ExprCtx, f.TypeCtx, *forbidden, *f.FuncRegistry);
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
        NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
        TGlobalInliningStage inlining("native inlining");
        inlining.RunStage(root, nativeContext);
        TGlobalPruningStage pruning("native pruning");
        pruning.RunStage(root, nativeContext);
        UNIT_ASSERT(root.ComputeTypes(nativeContext) == IGraphTransformer::TStatus::Ok);
        for (const auto& item : root) for (const auto& expression : item.Current->GetExpressions()) {
            UNIT_ASSERT(expression.get().IsNative());
            UNIT_ASSERT(!expression.get().Node);
        }
    }

    Y_UNIT_TEST(AggregationAndUnionHaveNativeInputs) {
        NTests::TIdTestContext f;
        auto roots = CompileNativeToRbo(R"(
            SELECT SUM(x) AS total, COUNT(*) AS n
            FROM (SELECT 1 AS x UNION ALL SELECT 2L AS x) AS q
            HAVING SUM(x) > 0
        )", NoTables, f.ExprCtx);
        UNIT_ASSERT(roots[0]->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
        NTests::AssertIdInvariants(*roots[0]->GetInput(), roots[0]->PlanProps);
    }

    Y_UNIT_TEST(RuntimeColumnsParametersAndNumericCasts) {
        using namespace NFrontend;
        TExpressionArena arena;
        const auto column = arena.Column(0, {ETypeKind::Int32, true});
        const auto converted = arena.Cast(column, {ETypeKind::Int64});
        const auto fallback = arena.Literal({ETypeKind::Int64}, std::int64_t{42});
        const TExprId alternatives[]{converted, fallback};
        const auto parameter = arena.Parameter("p", {ETypeKind::Uint64});
        const auto result = arena.Binary(EOpcode::Add, arena.Coalesce(alternatives), arena.Cast(parameter, {ETypeKind::Int64}));
        for (const bool present : {false, true}) for (const ui64 parameterValue : {ui64{5}, std::numeric_limits<ui64>::max()}) {
            TEvaluationContext native;
            native.Column = [&](TColumnId) { return TValue{{ETypeKind::Int32, true}, present ? TScalar{std::int64_t{3}} : TScalar{std::monostate{}}}; };
            native.Parameter = [&](std::string_view) { return TValue{{ETypeKind::Uint64}, parameterValue}; };
            const auto expected = Evaluate(arena, result, native);
            TExprContext ctx;
            const auto lambda = LowerNativeLambda(arena, result, ctx, {});
            UNIT_ASSERT(AreAllNodesTypeAnnotated(*lambda));
            UNIT_ASSERT(!NeedsRboCompatibilityLowering(lambda));
            NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
            auto& pb = *setup.PgmBuilder;
            auto value = present ? pb.NewOptional(pb.NewDataLiteral<i32>(3)) :
                pb.NewEmptyOptional(pb.NewOptionalType(pb.NewDataType(NUdf::TDataType<i32>::Id)));
            auto row = pb.NewStruct({{"0", value}});
            NYql::NCommon::TMkqlCommonCallableCompiler compiler;
            NYql::NCommon::TMkqlBuildContext build(compiler, pb, ctx);
            build.Parameters = pb.NewStruct({{"$p", pb.NewDataLiteral<ui64>(parameterValue)}});
            auto graph = setup.BuildGraph(NYql::NCommon::MkqlBuildLambda(*lambda, build, {row}));
            auto actual = graph->GetValue();
            if (expected.IsNull()) UNIT_ASSERT(!actual);
            else UNIT_ASSERT_VALUES_EQUAL(actual.Get<i64>(), std::get<std::int64_t>(expected.Data));
        }
    }

    Y_UNIT_TEST(TerminalScalarExecution) {
        TString failures;
        for (auto sql : {
            "SELECT 1 + 2 * 3", "SELECT 1 / 0", "SELECT 7 / 2", "SELECT 9 % 4",
            "SELECT 1.0 / 0.0", "SELECT 0.0 / 0.0", "SELECT -1.0 / 0.0", "SELECT 7.0 / 2.0", "SELECT -3 + 2L",
            "SELECT CAST('123' AS Int64)", "SELECT CAST('bad' AS Int32)",
            "SELECT CAST(1 AS Int64)", "SELECT CAST(1 AS Uint8)",
            "SELECT COALESCE(NULL, CAST('bad' AS Int32), 7)",
            "SELECT CASE WHEN true THEN 7 ELSE 1/0 END",
            "SELECT 2 IN (1, NULL)", "SELECT NULL IS NULL", "SELECT NULL == NULL", "SELECT NULL AND NULL", "SELECT 4 IS NOT NULL",
            "SELECT Length('abcd')", "SELECT Contains('abcd', 'bc')",
            "SELECT StartsWith('abcd', 'ab')", "SELECT EndsWith('abcd', 'cd')", "SELECT 'a' || 'b'",
            "SELECT ABS(-3)", "SELECT NOT false", "SELECT true AND NULL", "SELECT true OR NULL",
            "SELECT Date('2020-01-02')", "SELECT Timestamp('2020-01-02T03:04:05Z')",
            "SELECT CAST('12.34' AS Decimal(10,2))",
            "SELECT CAST('1.23' AS Decimal(10,2)) / CAST('2' AS Decimal(10,2))",
            "SELECT CAST('1.23' AS Decimal(10,2)) * CAST('2' AS Decimal(10,2))",
            "SELECT CAST('999' AS Decimal(2,0))",
            "SELECT CAST(CAST('9.9' AS Decimal(2,1)) AS Int32)",
            "SELECT CAST(CAST('0.9' AS Decimal(1,1)) AS Int32)",
            "SELECT Date('2020-01-01') + Interval('P1D')",
            "SELECT CAST(Date('2020-01-02') AS Timestamp)",
            "SELECT Lower('AbC')", "SELECT Upper('AbC')", "SELECT Lower(CAST(NULL AS String))",
            "SELECT Substring('abcdef', 2, 3)", "SELECT Substring('abcdef', NULL, 3)",
            "SELECT Substring('abcdef', 4294967295u, 3)", "SELECT Substring('abcdef', 2, NULL)",
            "SELECT Substring('abcdef', IF(false, 2u), IF(false, 3u))",
            "SELECT CAST(12 AS Double)", "SELECT CAST(12 AS Bool)", "SELECT CAST(true AS Int32)",
            "SELECT CAST(9223372586610589697ul AS Float)", "SELECT CAST(4611686293305294849L AS Float)",
            "SELECT CAST(Double('-4.9406564584124654e-324') AS Uint64)",
            "SELECT CAST(Float('-1e-40') AS Uint8)",
            "SELECT CAST(12 AS String)", "SELECT CAST(Utf8('hello') AS String)",
            "SELECT 'abc' LIKE '%b%'"
        }) {
            try {
                CheckScalar(sql);
            } catch (const std::exception& error) {
                failures += TStringBuilder() << sql << ": " << error.what() << "\n";
            }
        }
        UNIT_ASSERT_C(failures.empty(), failures);
    }
}
} // namespace NKikimr::NKqp
