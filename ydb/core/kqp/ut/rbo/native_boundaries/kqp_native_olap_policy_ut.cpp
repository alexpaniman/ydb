#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/physical/predicate_collector.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

Y_UNIT_TEST_SUITE(KqpNativeOlapPolicy) {
    Y_UNIT_TEST(ComparisonEligibilityMatchesExistingCollector) {
        using namespace NFrontend;
        using namespace NYql;
        const std::pair<EOpcode, TStringBuf> comparisons[]{
            {EOpcode::Equal, "=="}, {EOpcode::NotEqual, "!="}, {EOpcode::Less, "<"},
            {EOpcode::LessEqual, "<="}, {EOpcode::Greater, ">"}, {EOpcode::GreaterEqual, ">="},
        };
        TString mismatches;
        for (const auto name : {"Bool", "Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32",
                "Int64", "Uint64", "Float", "Double", "String", "Utf8", "Uuid", "Decimal(12,2)",
                "Date", "Datetime", "Timestamp", "Interval", "Date32", "Datetime64", "Timestamp64", "Interval64"})
        for (const bool nullable : {false, true}) for (const bool same : {false, true})
        for (const auto& [op, callable] : comparisons) {
            TExpressionArena arena;
            const auto type = nullable ? ParseType(name).Optional() : ParseType(name);
            const auto predicate = arena.Binary(op, arena.Column(0, type), arena.Column(same ? 0 : 1, type));
            // Construct the legacy scalar boundary independently: typed row
            // members and a comparison, without going through native emission.
            TExprContext ctx;
            const TPositionHandle pos;
            const auto* fieldType = ExportNativeType(type, ctx);
            const auto* rowType = ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{
                ctx.MakeType<TItemExprType>("x", fieldType), ctx.MakeType<TItemExprType>("y", fieldType)});
            const auto row = ctx.NewArgument(pos, "row"); row->SetTypeAnn(rowType);
            const auto member = [&](TStringBuf field) {
                auto node = ctx.NewCallable(pos, "Member", {row, ctx.NewAtom(pos, field)});
                node->SetTypeAnn(fieldType);
                return node;
            };
            const auto legacy = ctx.NewCallable(pos, callable, {member("x"), member(same ? "x" : "y")});
            legacy->SetTypeAnn(ExportNativeType(arena[predicate].Type, ctx));
            for (const bool apply : {false, true}) {
                NOpt::TOLAPPredicateNode collected;
                NOpt::CollectPredicates(NNodes::TExprBase(legacy), collected, row.Get(), rowType, {apply, false});
                const bool expected = apply ? collected.CanBePushedApply : collected.CanBePushed;
                const bool actual = CanPushNativeOlapPredicate(arena, predicate, apply);
                UNIT_ASSERT_VALUES_EQUAL_C(CanLowerNativeOlapPredicate(arena, predicate, apply), actual,
                    type.ToString() << " " << callable << " scalarApply=" << apply);
                if (actual != expected) mismatches += TStringBuilder() << type.ToString() << " " << callable
                    << " same=" << same << " scalarApply=" << apply << " native=" << actual << " legacy=" << expected << '\n';
                if (actual && expected) {
                    const auto lowered = LowerNativeOlapPredicate(arena, predicate, ctx, {});
                    const bool scalar = !FindNodes(lowered, [](const auto& node) { return node->IsCallable("KqpOlapApply"); }).empty();
                    UNIT_ASSERT_VALUES_EQUAL_C(scalar, !collected.CanBePushed, type.ToString() << " " << callable);
                }
            }
        }
        UNIT_ASSERT_C(mismatches.empty(), mismatches);
    }
}
} // namespace NKikimr::NKqp
