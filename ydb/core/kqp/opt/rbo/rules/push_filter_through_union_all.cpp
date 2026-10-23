#include "kqp_rules_include.h"

namespace NKikimr::NKqp {
namespace {

bool IsTotalUnionPredicate(const TExpression& expression) {
    // Keep the first implementation to total primitive predicates. In particular,
    // do not move UDFs, volatile calls or expressions that can raise row errors.
    return !FindNode(expression.Node, [](const TExprNode::TPtr& node) {
        return node->IsCallable() && !node->IsCallable({
            "Member", "Parameter", "And", "Or", "Not", "Coalesce", "Exists", "Just", "Nothing",
            "==", "!=", "<", "<=", ">", ">=", "DataType", "OptionalType",
            "Bool", "Int8", "Int16", "Int32", "Int64", "Uint8", "Uint16", "Uint32", "Uint64",
            "Float", "Double", "Decimal", "String", "Utf8", "Date", "Datetime", "Timestamp",
            "Interval", "Date32", "Datetime64", "Timestamp64", "Interval64"});
    });
}

} // namespace

bool TPushFilterThroughUnionAllRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter && input->GetChildren().front()->Kind == EOperator::UnionAll;
}

TIntrusivePtr<IOperator> TPushFilterThroughUnionAllRule::SimpleMatchAndApply(
    const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props)
{
    const auto filter = CastOperator<TOpFilter>(input);
    const auto unionAll = CastOperator<TOpUnionAll>(filter->GetInput());
    const auto& predicate = filter->GetFilterExpression();
    if (filter->PartiallyPushedDown || ReferencesUnresolvedSubplan(predicate, props) ||
        !IsTotalUnionPredicate(predicate)) {
        return input;
    }
    const auto used = predicate.GetInputIUs(false, true);
    if (!used.IsSubsetOf(unionAll->GetColumns().Keys())) {
        return input;
    }

    TVector<TIntrusivePtr<IOperator>> children;
    children.reserve(unionAll->GetChildCount());
    for (size_t index = 0; index < unionAll->GetChildCount(); ++index) {
        const auto& child = unionAll->GetInput(index);
        TSubstitutions renames;
        for (const auto output : used) {
            const auto* row = unionAll->GetColumns().Find(output);
            Y_ENSURE(row && index < row->Inputs.size());
            const auto source = row->Inputs[index];
            const auto* outputType = unionAll->GetIUType(output, ctx.ExprCtx);
            const auto* inputType = child->GetIUType(source, ctx.ExprCtx);
            // Optional widening needs a typed expression adapter, not just ID replacement.
            if (!outputType || !inputType || !IsSameAnnotation(*outputType, *inputType)) {
                return input;
            }
            renames.Add(output, source);
        }
        children.push_back(MakeIntrusive<TOpFilter>(child, filter->Pos, predicate.ApplyRenames(renames)));
    }
    return MakeIntrusive<TOpUnionAll>(std::move(children), unionAll->Pos, unionAll->GetColumns(), unionAll->Ordered);
}

} // namespace NKikimr::NKqp
