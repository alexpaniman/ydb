#include "kqp_operator.h"
#include "kqp_native_types.h"

namespace NKikimr::NKqp {
using TStatus = NYql::IGraphTransformer::TStatus;

TStatus TOpRoot::ComputeTypes(TRBOContext& ctx) {
    // Intrusive references control lifetime, not logical consumers. Count each
    // actual edge, including packed CBO trees and registered subplan roots.
    absl::flat_hash_map<const IOperator*, const TReplicate*> visited;
    TUnorderedIUs subplans;
    TVector<std::pair<IOperator*, const IOperator*>> pending{{this, nullptr}};
    while (!pending.empty()) {
        const auto [op, parent] = pending.back();
        pending.pop_back();
        Y_ENSURE(op, "Null RBO input");
        // Repeated child views are legal only when they refer to the same
        // canonical input slot in a shared replication binding.
        const auto* replicate = parent && parent->Kind == EOperator::Replicate
            ? &CastOperator<TOpReplicate>(*parent).GetReplicate() : nullptr;
        const auto [it, inserted] = visited.emplace(op, replicate);
        if (!inserted) {
            if (!replicate || it->second != replicate) {
                ctx.ExprCtx.AddError(TIssue(ctx.ExprCtx.GetPosition(op->Pos),
                    TStringBuilder() << "Shared RBO " << op->GetExplainName()
                        << ": sharing requires distinct ports of the same Replicate"));
                return TStatus::Error;
            }
            continue;
        }
        if (op->Kind == EOperator::CBOTree) {
            // Its virtual children are views of boundary slots, not extra edges.
            pending.emplace_back(CastOperator<TOpCBOTree>(op)->TreeRoot.Get(), op);
        } else {
            for (auto* child : op->GetChildren()) {
                pending.emplace_back(child, op);
            }
        }
        for (const auto call : op->GetSubplanIUs(PlanProps.Subplans)) {
            if (subplans.Add(call)) {
                pending.emplace_back(PlanProps.Subplans.At(call).Plan.Get(), nullptr);
            }
        }
    }

    for (const auto& item : *this) {
        ComputeNativeTypes(*item.Current, PlanProps);
    }
    return TStatus::Ok;
}

} // namespace NKikimr::NKqp
