#include "kqp_native_scalar_test_helpers.h"
#include <ydb/core/kqp/opt/rbo/frontend/bindings.h>

namespace NKikimr::NKqp {
Y_UNIT_TEST_SUITE(KqpNativeBindingBoundaries) {
    Y_UNIT_TEST(ExternalBindingsExecuteWithoutSemanticPasses) {
        using namespace NFrontend;
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            auto source = std::make_shared<TExpressionArena>();
            const auto argument = source->Column(300, {ETypeKind::Int64});
            const TColumnId parameters[]{300};
            auto bindings = std::make_shared<TExternalBindings>();
            bindings->Callables.emplace("f", TScalarBinding(source, argument, parameters));
            bindings->Callables.emplace("Demo::Increment", TScalarBinding(source,
                source->Binary(EOpcode::Add, argument, source->Literal({ETypeKind::Int64}, std::int64_t{1})), parameters));
            TCompilerSettings settings;
            settings.Parser = parser;
            settings.Binding.ExternalBindings = bindings;
            auto compiled = CompileNative("DECLARE $x AS Int64?; SELECT $f($x) + Demo::Increment($x);", {}, settings);
            auto program = std::move(*compiled.Program).Release();
            for (const TScalar& value : {TScalar{}, TScalar{std::int64_t{-7}}, TScalar{std::int64_t{0}}, TScalar{std::int64_t{19}}}) {
                TEvaluationContext context;
                context.Parameter = [&](std::string_view name) -> TValue {
                    UNIT_ASSERT_VALUES_EQUAL(name, "x");
                    return {{ETypeKind::Int64, true}, value};
                };
                NTests::CheckScalar(program.Expressions, NTests::Projection(program), "external native bindings", context,
                    [&](NYql::NCommon::TMkqlBuildContext& build, NMiniKQL::TProgramBuilder& pb) {
                        const auto literal = program.Expressions.Literal({ETypeKind::Int64, true}, value);
                        const auto emitted = LowerNativeLambda(program.Expressions, literal, build.ExprCtx, {})->TailPtr();
                        build.Parameters = pb.NewStruct({{"$x", NYql::NCommon::MkqlBuildExpr(*emitted, build)}});
                    });
            }
        }
    }
}
} // namespace NKikimr::NKqp
