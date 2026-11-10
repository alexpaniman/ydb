#include <ydb/core/kqp/opt/rbo/kqp_expression.h>

#include <library/cpp/testing/unittest/registar.h>
#include <yql/essentials/public/decimal/yql_decimal.h>

namespace NKikimr::NKqp {
namespace {
using K = NFrontend::ETypeKind;
using O = NFrontend::EOpcode;
using TArena = NFrontend::TExpressionArena;
}

Y_UNIT_TEST_SUITE(KqpNativeExpressionArena) {
    Y_UNIT_TEST(DecimalTextCastsMatchRuntimeWithoutYqlEvaluation) {
        using namespace NFrontend;
        TVector<std::string> inputs{"", "+", "-", ".", "+.", "-.", "0", "42", "-7", "invalid", " 1", "1 ",
            "1.2x", "1..2", "1.235", "1.245", "-1.235", "-1.245", "inf", "-INF", "NaN", "+nan",
            "1e2", "1e-2", "1E+2", "1e+-2", "1e", "e2", "infE2", "nanE2", "99999999999999999999999999999999999999999999x"};
        for (const auto* mantissa : {"0", "1", "-1", "123.45", "-999.995"})
            for (int exponent : {-35, -9, -2, -1, 0, 1, 2, 9, 35})
                inputs.push_back(std::string(mantissa) + "e" + std::to_string(exponent));
        for (const auto type : {TType{K::Decimal, false, 1, 0}, TType{K::Decimal, false, 7, 0},
                TType{K::Decimal, false, 7, 2}, TType{K::Decimal, false, 7, 7}, TType{K::Decimal, false, 35, 12}}) {
            TArena arena;
            const auto input = arena.Column(1, {K::String, true});
            const auto cast = arena.Cast(input, type);
            for (const auto& text : inputs) {
                const auto expected = NYql::NDecimal::FromStringEx(TStringBuf(text.data(), text.size()), type.Precision, type.Scale);
                const auto check = [&](const TValue& actual) {
                    const TString detail = TStringBuilder() << type.ToString() << ": " << text;
                    UNIT_ASSERT_VALUES_EQUAL_C(actual.IsNull(), NYql::NDecimal::IsError(expected), detail);
                    if (!actual.IsNull()) UNIT_ASSERT_C(std::get<TDecimal>(actual.Data).Coefficient == expected, detail);
                };
                TEvaluationContext context;
                context.Column = [&](TColumnId) { return TValue{{K::String, true}, text}; };
                check(Evaluate(arena, cast, context));
                check(Evaluate(arena, arena.Cast(arena.Literal({K::String}, text), type)));
                const auto bound = arena.RewriteColumns(cast, [&](TExprId, TColumnId, TType type, TSourcePosition pos) {
                    return arena.Literal(type, text, pos);
                });
                const TExprId roots[]{bound};
                check(Evaluate(arena, arena.FoldConstants(roots).front()));
            }
        }
    }

    Y_UNIT_TEST(CompositionAppendsToTheQueryArena) {
        TPlanProps props;
        auto arena = std::make_shared<TArena>();
        const auto column = arena->Column(1, {K::Bool});
        auto original = TExpression::FromNative(arena, column, &props);
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

        NFrontend::TEvaluationContext context;
        context.Column = [](NFrontend::TColumnId) { return NFrontend::TValue{{K::Bool}, true}; };
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*arena, disjunction.GetNativeRoot(), context)), "true");
    }

    Y_UNIT_TEST(RenamesAndRetypingPreserveOriginalHandles) {
        TPlanProps props;
        auto arena = std::make_shared<TArena>();
        const auto column = arena->Column(1, {K::Int32});
        const auto constant = arena->Literal({K::Int32}, std::int64_t{7});
        const auto sum = arena->Binary(O::Add, column, constant);
        auto original = TExpression::FromNative(arena, sum, &props);
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

    }

    Y_UNIT_TEST(AssertionsUseNativeOperands) {
        auto arena = std::make_shared<TArena>();
        const auto value = arena->Literal({K::Int32}, std::int64_t{7});
        const auto predicate = arena->Literal({K::Bool}, true);
        auto expression = MakeEnsure(TExpression::FromNative(arena, value),
            TExpression::FromNative(arena, predicate), "cardinality");
        UNIT_ASSERT(expression.IsNative());

        static_assert(!std::is_constructible_v<TExpression, TExprNode::TPtr, TExprContext*>);
        UNIT_ASSERT(expression.GetNativeArena() == arena);
        UNIT_ASSERT_VALUES_EQUAL(arena->Size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*arena, expression.GetNativeRoot())), "7");
    }

    Y_UNIT_TEST(ForeignArenaCompositionImportsOnlyTheForeignOperand) {
        auto leftArena = std::make_shared<TArena>();
        auto rightArena = std::make_shared<TArena>();
        const auto leftId = leftArena->Column(1, {K::Bool});
        rightArena->Literal({K::Int32}, std::int64_t{7}); // Unreachable nodes must not be imported.
        const auto rightId = rightArena->Literal({K::Bool}, true);
        auto left = TExpression::FromNative(leftArena, leftId);
        auto right = TExpression::FromNative(rightArena, rightId);
        auto combined = MakeBinaryPredicate("Or", left, right);
        UNIT_ASSERT(combined.GetNativeArena() == leftArena);
        UNIT_ASSERT_VALUES_EQUAL(leftArena->Size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(rightArena->Size(), 2);
        UNIT_ASSERT_VALUES_EQUAL((*leftArena)[combined.GetNativeRoot()].Arguments[0], leftId);
        NFrontend::TEvaluationContext context;
        context.Column = [](NFrontend::TColumnId) { return NFrontend::TValue{{K::Bool}, false}; };
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*leftArena, combined.GetNativeRoot(), context)), "true");
        std::weak_ptr<TArena> lifetime = leftArena;
        left = {};
        leftArena.reset();
        UNIT_ASSERT(!lifetime.expired());
        UNIT_ASSERT_VALUES_EQUAL(NFrontend::FormatValue(NFrontend::Evaluate(*combined.GetNativeArena(), combined.GetNativeRoot(), context)), "true");
    }
}
} // namespace NKikimr::NKqp
