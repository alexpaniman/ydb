#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>

namespace NKikimr::NKqp {

TConstantFoldingStage::TConstantFoldingStage() : IRBOStage("Constant folding stage") {
    Props = ERuleProperties::RequireParents |  ERuleProperties::RequireTypes;
}

void TConstantFoldingStage::RunStage(TOpRoot& root, TRBOContext& ctx) {
    Y_UNUSED(ctx);
    THashMap<NFrontend::TExpressionArena*, TVector<const TExpression*>> nativeExpressions;
    TVector<IOperator*> nativeOps;
    for (const auto& item : root) {
        const auto expressions = item.Current->GetExpressions();
        if (expressions.empty()) continue;
        nativeOps.push_back(item.Current);
        for (const auto& expression : expressions)
            nativeExpressions[expression.get().GetNativeArena().get()].push_back(&expression.get());
    }
    THashMap<const TExpression*, TExpression> nativeReplacements;
    for (const auto& [arena, expressions] : nativeExpressions) {
        std::vector<NFrontend::TExprId> roots;
        roots.reserve(expressions.size());
        for (const auto* expression : expressions) roots.push_back(expression->GetNativeRoot());
        const auto folded = arena->FoldConstants(roots);
        for (std::size_t i = 0; i < expressions.size(); ++i) {
            const auto& expression = *expressions[i];
            if (folded[i] != roots[i]) nativeReplacements.emplace(&expression,
                TExpression::FromNative(expression.GetNativeArena(), folded[i], expression.PlanProps, expression.Pos()));
        }
    }

    if (nativeReplacements.empty()) return;
    for (auto* op : nativeOps) op->RewriteExpressions([&](const TExpression& expression) {
        const auto replacement = nativeReplacements.find(&expression);
        return replacement == nativeReplacements.end() ? expression : replacement->second;
    });

}

} // namespace NKikimr::NKqp
