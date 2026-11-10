#pragma once

#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_compatibility.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/minikql/comp_nodes/ut/mkql_computation_node_ut.h>
#include <yql/essentials/providers/common/mkql/yql_provider_mkql.h>
#include <library/cpp/testing/unittest/registar.h>

#include <cmath>

namespace NKikimr::NKqp::NTests {
inline NFrontend::TExprId Projection(const NFrontend::TBoundProgram& program) {
    const auto column = program.Results.at(0).Columns.at(0).Column;
    for (const auto& relation : program.Relations) {
        for (const auto& definition : relation.Definitions) if (definition.Column == column) return definition.Expression;
    }
    ythrow yexception() << "projection not found";
}

// Compile and execute the terminal scalar graph directly: no YQL annotation,
// expression peepholes, SQL translation or compatibility transformer.
inline void CheckScalar(const NFrontend::TExpressionArena& arena, NFrontend::TExprId id, std::string_view sql,
    const NFrontend::TEvaluationContext& evaluation = {},
    const std::function<void(NYql::NCommon::TMkqlBuildContext&, NMiniKQL::TProgramBuilder&)>& bind = {}) {
    const auto expected = NFrontend::Evaluate(arena, id, evaluation);
    NYql::TExprContext ctx;
    const auto lambda = LowerNativeLambda(arena, id, ctx, {});
    UNIT_ASSERT_C(AreAllNodesTypeAnnotated(*lambda), sql);
    UNIT_ASSERT_C(!NeedsRboCompatibilityLowering(lambda), sql);
    NMiniKQL::TSetup<false> setup(NMiniKQL::GetBuiltinFactory());
    auto registry = setup.FunctionRegistry->Clone();
    NMiniKQL::FillStaticModules(*registry);
    setup.FunctionRegistry = registry;
    setup.PgmBuilder.Reset(new NMiniKQL::TProgramBuilder(*setup.Env, *registry));
    NYql::NCommon::TMkqlCommonCallableCompiler compiler;
    NYql::NCommon::TMkqlBuildContext build(compiler, *setup.PgmBuilder, ctx);
    if (bind) bind(build, *setup.PgmBuilder);
    const auto runtime = NYql::NCommon::MkqlBuildLambda(*lambda, build, {setup.PgmBuilder->NewEmptyStruct()});
    auto graph = setup.BuildGraph(runtime);
    const auto value = graph->GetValue();
    const auto checkValue = [&](auto&& self, const NUdf::TUnboxedValuePod& value, const NFrontend::TValue& expected) -> void {
        if (expected.IsNull()) {
            UNIT_ASSERT_C(!value, sql);
            return;
        }
        UNIT_ASSERT_C(value, sql);
        if (expected.Type.OptionalDepth > 1) {
            self(self, value.GetOptionalValue(), *std::get<NFrontend::TOptionalValue>(expected.Data));
            return;
        }
        using K = NFrontend::ETypeKind;
        switch (expected.Type.Kind) {
            case K::Bool: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<bool>(), std::get<bool>(expected.Data), sql); break;
            case K::Int8: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i8>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Int16: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i16>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Int32: case K::Date32: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i32>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Int64: case K::Interval: case K::Datetime64: case K::Timestamp64: case K::Interval64:
                UNIT_ASSERT_VALUES_EQUAL_C(value.Get<i64>(), std::get<std::int64_t>(expected.Data), sql); break;
            case K::Uint8: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui8>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Uint16: case K::Date: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui16>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Uint32: case K::Datetime: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui32>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Uint64: case K::Timestamp: UNIT_ASSERT_VALUES_EQUAL_C(value.Get<ui64>(), std::get<std::uint64_t>(expected.Data), sql); break;
            case K::Float: case K::Double: {
                const auto actual = expected.Type.Kind == K::Float ? double(value.Get<float>()) : value.Get<double>();
                const auto number = std::get<double>(expected.Data);
                UNIT_ASSERT_C((std::isnan(actual) && std::isnan(number)) || actual == number, sql);
                if (actual == 0) UNIT_ASSERT_VALUES_EQUAL_C(std::signbit(actual), std::signbit(number), sql);
                break;
            }
            case K::String: case K::Utf8: case K::Uuid: UNIT_ASSERT_VALUES_EQUAL_C(TString(value.AsStringRef()), TString(std::get<std::string>(expected.Data)), sql); break;
            case K::Decimal: UNIT_ASSERT_C(value.GetInt128() == std::get<NFrontend::TDecimal>(expected.Data).Coefficient, sql); break;
            case K::Null: UNIT_FAIL("unexpected non-null Null");
            case K::Tuple: {
                const auto& items = std::get<NFrontend::TTuple>(expected.Data)->Items;
                for (std::size_t i = 0; i < items.size(); ++i) self(self, value.GetElement(i), items[i]);
                break;
            }
            case K::List: UNIT_FAIL("collection values are only IN operands");
            case K::Calendar: UNIT_FAIL("calendar values must be consumed by scalar date operations");
            case K::Json: case K::JsonDocument: UNIT_FAIL("JSON values must be consumed by scalar JSON operations");
        }
    };
    checkValue(checkValue, value, expected);
}
inline void CheckScalar(std::string_view sql) {
    auto program = NFrontend::NDetail::Compile(sql, [](auto, auto pos) -> NFrontend::TCatalogTable {
        throw NFrontend::TFrontendError(pos, "unexpected table");
    });
    CheckScalar(program.Expressions, Projection(program), sql);
}

} // namespace NKikimr::NKqp::NTests
