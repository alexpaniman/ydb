#include <ydb/core/kqp/opt/rbo/kqp_expression.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {
namespace {
using K = NFrontend::ETypeKind;
using O = NFrontend::EOpcode;
using TArena = NFrontend::TExpressionArena;
}

Y_UNIT_TEST_SUITE(KqpNativeExpressionArena) {
    Y_UNIT_TEST(CompositionAppendsToTheQueryArena) {
        TExprContext ctx;
        TPlanProps props;
        auto arena = std::make_shared<TArena>();
        const auto column = arena->Column(1, {K::Bool});
        auto original = TExpression::FromNative(arena, column, &ctx, &props);
        auto expression = original;
        for (unsigned i = 0; i < 1000; ++i) {
            const auto previous = expression.GetNativeRoot();
            expression = MakeNegation(expression);
            UNIT_ASSERT(expression.GetNativeArena() == arena);
            UNIT_ASSERT_VALUES_EQUAL((*arena)[expression.GetNativeRoot()].Arguments[0], previous);
        }
        UNIT_ASSERT_VALUES_EQUAL(arena->Size(), 1001);
        const auto before = arena->Size();
        auto exists = MakeUnaryCallable("Exists", original);
        auto conjunction = MakeConjunction({original, expression, exists});
        auto disjunction = MakeBinaryPredicate("Or", conjunction, original);
        UNIT_ASSERT_VALUES_EQUAL(arena->Size(), before + 4);
        UNIT_ASSERT(disjunction.GetNativeArena() == arena);
        UNIT_ASSERT_VALUES_EQUAL((*arena)[disjunction.GetNativeRoot()].Arguments[1], column);
        UNIT_ASSERT_VALUES_EQUAL(original.GetNativeRoot(), column);
        UNIT_ASSERT((*arena)[column].Op == O::Column);
        UNIT_ASSERT(!original.Node && !expression.Node && !disjunction.Node);
        NFrontend::TEvaluationContext context;
        context.Column = [](NFrontend::TColumnId) { return NFrontend::TValue{{K::Bool}, true}; };
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*arena, disjunction.GetNativeRoot(), context)), "true");
    }

    Y_UNIT_TEST(RenamesAndRetypingPreserveOriginalHandles) {
        TExprContext ctx;
        TPlanProps props;
        auto arena = std::make_shared<TArena>();
        const auto column = arena->Column(1, {K::Int32});
        const auto constant = arena->Literal({K::Int32}, std::int64_t{7});
        const auto sum = arena->Binary(O::Add, column, constant);
        auto original = TExpression::FromNative(arena, sum, &ctx, &props);
        UNIT_ASSERT(original.GetRawInputIUs().Contains(1));
        const auto before = arena->Size();
        auto renamed = original.ApplyRenames({{1, 2}});
        UNIT_ASSERT(renamed.GetNativeArena() == arena);
        UNIT_ASSERT_VALUES_EQUAL(arena->Size(), before + 2);
        UNIT_ASSERT_VALUES_EQUAL((*arena)[renamed.GetNativeRoot()].Arguments[1], constant);
        UNIT_ASSERT(renamed.GetRawInputIUs().Contains(2));
        UNIT_ASSERT(!renamed.GetRawInputIUs().Contains(1));
        UNIT_ASSERT(original.GetRawInputIUs().Contains(1));
        UNIT_ASSERT_VALUES_EQUAL((*arena)[column].ColumnId, 1);
        auto widened = renamed.WithNativeColumnTypes([](TInfoUnitId) { return NFrontend::TType{K::Int64}; });
        UNIT_ASSERT(widened.GetNativeArena() == arena);
        UNIT_ASSERT(widened.GetNativeType() == NFrontend::TType{K::Int64});
        UNIT_ASSERT(original.GetNativeType() == NFrontend::TType{K::Int32});
        const auto after = arena->Size();
        auto unchanged = widened.WithNativeColumnTypes([](TInfoUnitId) { return NFrontend::TType{K::Int64}; });
        UNIT_ASSERT_VALUES_EQUAL(unchanged.GetNativeRoot(), widened.GetNativeRoot());
        UNIT_ASSERT_VALUES_EQUAL(arena->Size(), after);
        auto absolute = MakeUnaryCallable("Abs", widened);
        UNIT_ASSERT(absolute.GetNativeArena() == arena);
        UNIT_ASSERT_VALUES_EQUAL(arena->Size(), after + 1);
        UNIT_ASSERT_VALUES_EQUAL((*arena)[absolute.GetNativeRoot()].Arguments[0], widened.GetNativeRoot());
        UNIT_ASSERT(!original.Node && !renamed.Node && !widened.Node);
    }

    Y_UNIT_TEST(ForeignArenaCompositionImportsOnlyTheForeignOperand) {
        TExprContext ctx;
        auto leftArena = std::make_shared<TArena>();
        auto rightArena = std::make_shared<TArena>();
        const auto leftId = leftArena->Literal({K::Bool}, false);
        const auto rightId = rightArena->Literal({K::Bool}, true);
        auto left = TExpression::FromNative(leftArena, leftId, &ctx);
        auto right = TExpression::FromNative(rightArena, rightId, &ctx);
        auto combined = MakeBinaryPredicate("Or", left, right);
        UNIT_ASSERT(combined.GetNativeArena() == leftArena);
        UNIT_ASSERT_VALUES_EQUAL(leftArena->Size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(rightArena->Size(), 1);
        UNIT_ASSERT_VALUES_EQUAL((*leftArena)[combined.GetNativeRoot()].Arguments[0], leftId);
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*leftArena, combined.GetNativeRoot())), "true");
        std::weak_ptr<TArena> lifetime = leftArena;
        left = {};
        leftArena.reset();
        UNIT_ASSERT(!lifetime.expired());
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*combined.GetNativeArena(), combined.GetNativeRoot())), "true");
    }
}
} // namespace NKikimr::NKqp
