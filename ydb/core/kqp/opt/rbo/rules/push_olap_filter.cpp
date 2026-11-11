#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_olap.h>
#include <ydb/core/kqp/provider/yql_kikimr_settings.h>

namespace NKikimr::NKqp {

namespace {

using namespace NKikimr;
using namespace NKikimr::NKqp;

bool IsSuitableToPushPredicateToColumnTables(IOperator* input) {
    if (input->Kind != EOperator::Filter) {
        return false;
    }

    const auto filter = CastOperator<TOpFilter>(input);

    const auto maybeRead = filter->GetInput().Get();
    if (maybeRead->Kind != EOperator::Source){
        return false;
    }

    const auto read = CastOperator<TOpRead>(maybeRead);
    return read->GetTableStorageType() == NYql::EStorageType::ColumnStorage && !read->OlapProgram;
}

} // anonymous namespace

bool TPushOlapFilterRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter &&
        input->GetChildren().front()->Kind == EOperator::Source;
}

TIntrusivePtr<IOperator> TPushOlapFilterRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps&) {
    if (!ctx.KqpCtx.Config->HasOptEnableOlapPushdown()) {
        return input;
    }

    if (!IsSuitableToPushPredicateToColumnTables(input.get())) {
        return input;
    }

    const auto filter = CastOperator<TOpFilter>(input);
    const auto read = CastOperator<TOpRead>(filter->GetInput().Get());

    const auto& expression = filter->GetFilterExpression();
    // Both the executable program and its saved statistics predicate must
    // bind entirely to this read's row.
    if (!expression.GetRawInputIUs().IsSubsetOf(read->GetColumns())) return input;
    const auto& arena = *expression.GetNativeArena();
    THashSet<NFrontend::TExprId> seen;
    TVector<NFrontend::TExprId> pending{expression.GetNativeRoot()};
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!seen.insert(id).second) continue;
        const auto& node = arena[id];
        // A storage filter discards NULL. A nullable conjunct followed by
        // a checked expression still evaluates that expression in SQL,
        // so even pushing a leading conjunct could suppress its error.
        if (node.Op == NFrontend::EOpcode::Ensure || node.Op == NFrontend::EOpcode::Unwrap) return input;
        pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
    }
    TVector<TExpression> pushed, remaining;
    for (const auto& term : filter->GetFilterExpression().SplitConjunct()) {
        // The checked-expression guard above makes reordering these total
        // conjuncts safe, including a pushable term after a compute term.
        if (CanPushNativeOlapPredicate(*term.GetNativeArena(), term.GetNativeRoot(),
            ctx.KqpCtx.Config->GetEnableOlapScalarApply(), ctx.KqpCtx.Config->GetEnableOlapPushdownRegexp())) pushed.push_back(term);
        else remaining.push_back(term);
    }
    if (pushed.empty()) return input;
    const auto predicate = MakeConjunction(pushed);
    auto newRead = MakeIntrusive<TOpRead>(read->Alias, read->GetColumns(), read->StorageType, read->TableRef,
        TOlapProgram(predicate), read->Limit, read->RangeInfo,
        read->OriginalPredicate ? read->OriginalPredicate : std::optional{filter->GetFilterExpression()},
        read->SortDir, read->Props, read->Pos);
    if (remaining.empty()) return newRead;
    return MakeIntrusive<TOpFilter>(newRead, filter->Pos, filter->Props, MakeConjunction(remaining), true);
}
} // namespace NKikimr::NKqp
