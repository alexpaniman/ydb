#include "kqp_rules_include.h"
#include <ydb/core/kqp/opt/rbo/kqp_subplan_value.h>

namespace NKikimr::NKqp {

bool TInlineGenericInExistsSubplanRule::QuickMatch(const TIntrusivePtr<IOperator>& input, const TPlanProps& props) const {
    if (input->Kind != EOperator::Filter || props.Subplans.Empty()) {
        return false;
    }

    for (const auto& iu : input->GetSubplanIUs(props.Subplans)) {
        const auto type = props.Subplans.At(iu).Type;
        if (type == ESubplanType::IN_SUBPLAN || type == ESubplanType::EXISTS) {
            return true;
        }
    }

    return false;
}

bool TInlineGenericInExistsSubplanRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter;
}

TIntrusivePtr<IOperator> TInlineGenericInExistsSubplanRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    if (input->Kind != EOperator::Filter) {
        return input;
    }

    // Check that the filter contains at least one IN/EXISTS subplan.
    auto consumer = CastOperator<TOpFilter>(input);
    TVector<TInfoUnitId> inOrExistsSubplans;

    for (const auto& subplanIU : consumer->GetSubplanIUs(props.Subplans)) {
        const auto type = props.Subplans.At(subplanIU).Type;
        if (type == ESubplanType::IN_SUBPLAN || type == ESubplanType::EXISTS) {
            inOrExistsSubplans.push_back(subplanIU);
        }
    }

    if (inOrExistsSubplans.empty()) {
        return input;
    }

    // Now we will pick the first subplan IU and join its subplan before filter
    // Then we'll remove the subplan from subplans list and rebuild the filter expression
    // so the current iu is no longer marked as SubplanIU

    auto subplanIU = inOrExistsSubplans[0];
    const bool nativeEqualNulls =
        ctx.KqpCtx.Config->GetUseBlockHashJoin() && ctx.KqpCtx.Config->GetEnableBlockHashJoinEqualNulls();
    auto newFilterInput = BuildInExistsValue(consumer->GetInput(), subplanIU,
        consumer->Pos, props, nativeEqualNulls);
    auto expression = consumer->GetFilterExpression();
    expression.BindPlanProps(&props);
    return MakeIntrusive<TOpFilter>(newFilterInput, consumer->Pos, std::move(expression));
}

} // namespace NKikimr::NKqp
