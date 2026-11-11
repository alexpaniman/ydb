#include "kqp_native_scalar_test_helpers.h"

namespace NKikimr::NKqp {
Y_UNIT_TEST_SUITE(KqpNativeStructBoundaries) {
    Y_UNIT_TEST(ConstructionAndMembersExecuteWithoutSemanticPasses) {
        using namespace NFrontend;
        using K = ETypeKind;
        using TItem = std::pair<std::string, TExprId>;
        TExpressionArena arena;
        const TItem fields[]{{"z", arena.Literal({K::String}, std::string{"tail"})},
            {"some`field", arena.Literal({K::Int32}, std::int64_t{42})}, {"nullable", arena.Literal({K::Int64, true}, {})}};
        const auto record = arena.Struct(fields);
        const auto member = arena.Member(record, "some`field");
        const TItem nestedFields[]{{"record", record}};
        const auto nested = arena.Struct(nestedFields);
        const TExprId records[]{record, record};
        const auto list = arena.Collection(records, true);
        const TExprId roots[]{record, member, nested, list, arena.Struct({}), arena.Unary(EOpcode::HasNull, record)};
        const auto folded = arena.FoldConstants(roots);
        for (std::size_t i = 0; i < std::size(roots); ++i) {
            NTests::CheckScalar(arena, roots[i], "native struct value");
            NTests::CheckScalar(arena, folded[i], "folded native struct value");
        }
    }

    Y_UNIT_TEST(RuntimeRecordMembersPreserveMissingAndNullableFields) {
        using namespace NFrontend;
        using K = ETypeKind;
        using TItem = std::pair<std::string, TExprId>;
        for (const bool optionalRecord : {false, true}) for (const unsigned depth : {0u, 1u, 2u}) {
            TExpressionArena arena;
            auto value = arena.Literal({K::Int64}, std::int64_t{91});
            if (depth) value = arena.Literal({K::Int64, true}, {});
            if (depth == 2) {
                const TExprId args[]{value};
                value = arena.Call("Just", args);
            }
            const TItem fields[]{{"z", arena.Null()}, {"some`field", value}};
            const auto record = arena.Struct(fields);
            auto type = arena[record].Type;
            if (optionalRecord) type = type.Optional();
            const auto parameter = arena.Parameter("r", type);
            const auto member = arena.Member(parameter, "some`field");
            auto payload = Evaluate(arena, record).Data;
            for (const bool missing : {false, true}) {
                if (missing && !optionalRecord) continue;
                const TValue runtime{type, missing ? TScalar{} : payload};
                TEvaluationContext evaluation;
                evaluation.Parameter = [&](auto) { return runtime; };
                const auto bind = [&](NYql::NCommon::TMkqlBuildContext& build, NMiniKQL::TProgramBuilder& pb) {
                    const auto literal = arena.Literal(runtime.Type, runtime.Data);
                    const auto emitted = LowerNativeLambda(arena, literal, build.ExprCtx, {})->TailPtr();
                    build.Parameters = pb.NewStruct({{"$r", NYql::NCommon::MkqlBuildExpr(*emitted, build)}});
                };
                NTests::CheckScalar(arena, member, "native nullable struct member", evaluation, bind);
                const auto nullMember = arena.Member(parameter, "z");
                NTests::CheckScalar(arena, nullMember, "native Null struct member", evaluation, bind);
            }
        }
    }
}
} // namespace NKikimr::NKqp
