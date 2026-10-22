#include "kqp_rules_include.h"

namespace NKikimr {
namespace NKqp {

bool TFuseFiltersRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter &&
        input->GetChildren().front()->Kind == EOperator::Filter;
}

// Fuse consecutive filters while preserving their evaluation boundary.

TIntrusivePtr<IOperator> TFuseFiltersRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    if (input->Kind != EOperator::Filter) {
        return input;
    }

    auto topFilter = CastOperator<TOpFilter>(input);
    if (topFilter->GetInput()->Kind != Filter) {
        return input;
    }

    auto bottomFilter = CastOperator<TOpFilter>(topFilter->GetInput());

    auto rejected = ctx.ExprCtx.NewCallable(input->Pos, "Bool", {ctx.ExprCtx.NewAtom(input->Pos, "false")});
    auto filterPredicate = [&](const TExpression& expression) {
        auto predicate = expression.GetExpressionBody();
        const TPgExprType* pgType = nullptr;
        if (IsPg(RemoveOptionalType(expression.Node->GetTypeAnn()), pgType)) {
            predicate = ctx.ExprCtx.NewCallable(predicate->Pos(), "FromPg", {predicate});
        }
        return ctx.ExprCtx.NewCallable(predicate->Pos(), "Coalesce", {predicate, rejected});
    };

    // A filter rejects NULL as well as false. A plain AND could still evaluate
    // the outer predicate after NULL, even when its evaluation is not safe.
    auto fusedPredicate = ctx.ExprCtx.NewCallable(input->Pos, "If", {
        filterPredicate(bottomFilter->GetFilterExpression()),
        filterPredicate(topFilter->GetFilterExpression()),
        rejected,
    });

    return MakeIntrusive<TOpFilter>(bottomFilter->GetInput(), topFilter->Pos, topFilter->Props,
        TExpression(fusedPredicate, &ctx.ExprCtx, &props), topFilter->PartiallyPushedDown);
}
}
}
