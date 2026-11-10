#include "kqp_rbo_physical_layout.h"

#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_type_annotation.h>

#include <optional>

namespace NKikimr::NKqp {
namespace {
using namespace NYql;
using TLayoutTypes = THashMap<const TExprNode*, const TTypeAnnotationNode*>;

const TTypeAnnotationNode* TypeOf(const TExprNode& node, const TLayoutTypes& types) {
    if (const TTypeAnnotationNode* type = node.GetTypeAnn()) return type;
    const auto found = types.find(&node);
    return found == types.end() ? nullptr : found->second;
}

bool IsIdentityRowMap(const TExprNode& lambda) {
    if (lambda.ChildrenSize() != 2 || lambda.Head().ChildrenSize() != 1 || !lambda.Tail().IsCallable("AsStruct")) return false;
    const auto& argument = lambda.Head().Head();
    const TTypeAnnotationNode* type = argument.GetTypeAnn();
    if (!type || type->GetKind() != ETypeAnnotationKind::Struct) return false;
    const auto* rowType = type->Cast<TStructExprType>();
    if (rowType->GetSize() != lambda.Tail().ChildrenSize()) return false;
    for (const auto& field : lambda.Tail().Children()) {
        const auto& value = field->Tail();
        if (!rowType->FindItem(field->Head().Content()) || !value.IsCallable("Member") ||
            &value.Head() != &argument || value.Tail().Content() != field->Head().Content()) return false;
    }
    return true;
}

TExprNode::TPtr BlockSort(const TExprNode::TPtr& node, TExprContext& ctx,
    const TLayoutTypes& layouts, const TTypeAnnotationContext& types)
{
    const auto mode = types.UseBlocks ? EBlockEngineMode::Force : types.BlockEngineMode;
    if (mode == EBlockEngineMode::Disable || !types.ArrowResolver) return node;
    const auto& input = node->Head();
    const bool unpacked = input.IsCallable("ToFlow") && input.ChildrenSize() == 1 && input.Head().IsCallable("WideFromBlocks");
    if (mode == EBlockEngineMode::Auto && !unpacked) return node;
    const auto* type = TypeOf(input, layouts);
    if (!type || type->GetKind() != ETypeAnnotationKind::Flow) return node;
    const auto* item = type->Cast<TFlowExprType>()->GetItemType();
    if (item->GetKind() != ETypeAnnotationKind::Multi) return node;
    TVector<const TTypeAnnotationNode*> columns;
    for (const auto* column : item->Cast<TMultiExprType>()->GetItems()) {
        if (column->IsBlockOrScalar()) return node;
        columns.push_back(column);
    }
    const auto supported = types.ArrowResolver->AreTypesSupported(ctx.GetPosition(input.Pos()), columns, ctx);
    YQL_ENSURE(supported != IArrowResolver::ERROR);
    if (supported != IArrowResolver::OK) return node;

    auto children = node->ChildrenList();
    // Keep the selected sort/top and its keys/count. Only its row/block
    // implementation changes, under the existing backend execution policy.
    children[0] = unpacked
        ? ctx.NewCallable(node->Pos(), "ReplicateScalars", {input.Head().HeadPtr()})
        : ctx.NewCallable(node->Pos(), "WideToBlocks", {ctx.NewCallable(node->Pos(), "FromFlow", {node->HeadPtr()})});
    auto sorted = ctx.NewCallable(node->Pos(), TString(node->Content()) + "Blocks", std::move(children));
    return ctx.NewCallable(node->Pos(), "ToFlow", {ctx.NewCallable(node->Pos(), "WideFromBlocks", {std::move(sorted)})});
}

// Only argument selection is eligible. Computations, including volatile or
// throwing expressions, must remain in their original evaluation scope.
std::optional<TVector<size_t>> Projection(const TExprNode& lambda) {
    if (!lambda.IsLambda()) return {};
    THashMap<const TExprNode*, size_t> arguments;
    for (size_t i = 0; i < lambda.Head().ChildrenSize(); ++i) arguments.emplace(lambda.Head().Child(i), i);
    TVector<size_t> fields;
    for (size_t i = 1; i < lambda.ChildrenSize(); ++i) {
        const auto found = arguments.find(lambda.Child(i));
        if (found == arguments.end()) return {};
        fields.push_back(found->second);
    }
    return fields;
}

TExprNode::TPtr ProjectLambda(const TExprNode& source, const TVector<size_t>& fields, bool blocks, TExprContext& ctx) {
    TExprNode::TListType args, body;
    for (const auto& arg : source.Head().Children()) args.push_back(ctx.NewArgument(arg->Pos(), arg->Content()));
    if (blocks) args.push_back(ctx.NewArgument(source.Pos(), "block_length"));
    for (const auto field : fields) body.push_back(args.at(field));
    if (blocks) body.push_back(args.back());
    return ctx.NewLambda(source.Pos(), ctx.NewArguments(source.Pos(), std::move(args)), std::move(body));
}

TExprNode::TPtr BlockProjection(const TExprNode& source, TExprContext& ctx, const TTypeAnnotationContext& types) {
    if (const auto fields = Projection(source)) return ProjectLambda(source, *fields, true, ctx);
    const auto mode = types.UseBlocks ? EBlockEngineMode::Force : types.BlockEngineMode;
    if (mode == EBlockEngineMode::Disable || !types.ArrowResolver) return {};

    TNodeOnNodeOwnedMap values;
    TExprNode::TListType args, body;
    for (const auto& arg : source.Head().Children()) {
        args.push_back(ctx.NewArgument(arg->Pos(), arg->Content()));
        values.emplace(arg.Get(), args.back());
    }
    // These constructors only attach a literal or an optional validity layer.
    // General scalar computations stay on rows; they are never speculatively
    // moved, duplicated or evaluated once per block here.
    const auto convert = [&](const auto& self, const TExprNode::TPtr& value) -> TExprNode::TPtr {
        if (const auto found = values.find(value.Get()); found != values.end()) return found->second;
        TExprNode::TPtr block;
        if (value->IsCallable("Just")) {
            if (auto inner = self(self, value->HeadPtr())) block = ctx.NewCallable(value->Pos(), "BlockJust", {std::move(inner)});
        } else if (const TTypeAnnotationNode* type = value->GetTypeAnn(); type && type->GetKind() == ETypeAnnotationKind::Data &&
            value->IsCallable(type->Cast<TDataExprType>()->GetName())) {
            bool literal = true;
            for (const auto& child : value->Children()) literal &= child->IsAtom();
            if (literal) block = ctx.NewCallable(value->Pos(), "AsScalar", {value});
        }
        values.emplace(value.Get(), block);
        return block;
    };
    TVector<const TTypeAnnotationNode*> outputTypes;
    for (size_t i = 1; i < source.ChildrenSize(); ++i) {
        const auto value = source.ChildPtr(i);
        auto block = convert(convert, value);
        if (!block || !value->GetTypeAnn()) return {};
        body.push_back(std::move(block));
        outputTypes.push_back(value->GetTypeAnn());
    }
    const auto supported = types.ArrowResolver->AreTypesSupported(ctx.GetPosition(source.Pos()), outputTypes, ctx);
    YQL_ENSURE(supported != IArrowResolver::ERROR);
    if (supported != IArrowResolver::OK) return {};
    args.push_back(ctx.NewArgument(source.Pos(), "block_length"));
    body.push_back(args.back());
    return ctx.NewLambda(source.Pos(), ctx.NewArguments(source.Pos(), std::move(args)), std::move(body));
}

TExprNode::TPtr UnpackRow(const TExprNode::TPtr& node, TExprContext& ctx) {
    const auto& narrow = node->Head();
    const auto& pack = narrow.Tail();
    const auto& unpack = node->Tail();
    if (pack.ChildrenSize() != 2 || !pack.Tail().IsCallable("AsStruct") || unpack.Head().ChildrenSize() != 1) return node;
    THashMap<const TExprNode*, size_t> arguments;
    for (size_t i = 0; i < pack.Head().ChildrenSize(); ++i) arguments.emplace(pack.Head().Child(i), i);
    THashMap<TStringBuf, size_t> members;
    for (const auto& member : pack.Tail().Children()) {
        const auto found = arguments.find(member->Child(1));
        if (found == arguments.end()) return node;
        members.emplace(member->Head().Content(), found->second);
    }
    TVector<size_t> fields;
    for (size_t i = 1; i < unpack.ChildrenSize(); ++i) {
        const auto& member = *unpack.Child(i);
        if (!member.IsCallable("Member") || member.HeadPtr() != unpack.Head().HeadPtr()) return node;
        const auto found = members.find(member.Tail().Content());
        if (found == members.end()) return node;
        fields.push_back(found->second);
    }
    return ctx.NewCallable(node->Pos(), "WideMap", {narrow.HeadPtr(), ProjectLambda(pack, fields, false, ctx)});
}

TExprNode::TPtr Rewrite(const TExprNode::TPtr& node, TExprContext& ctx, const TLayoutTypes& types,
    const TTypeAnnotationContext& executionTypes)
{
    if (node->IsCallable({"Map", "OrderedMap"}) && IsIdentityRowMap(node->Tail())) return node->HeadPtr();
    if (node->IsCallable({"WideSort", "WideTop", "WideTopSort"})) return BlockSort(node, ctx, types, executionTypes);
    if (node->IsCallable("ToFlow") && node->ChildrenSize() == 1) {
        if (node->Head().IsCallable("FromFlow")) return node->Head().HeadPtr();
        if (node->Head().IsCallable("Iterator") && node->Head().ChildrenSize() == 1) {
            const auto* type = TypeOf(node->Head().Head(), types);
            // Iterator(Optional<List<T>>) flattens the list, whereas ToFlow
            // on that optional would produce one list item. Keep it intact.
            if (type && type->GetKind() == ETypeAnnotationKind::List) {
                return ctx.NewCallable(node->Pos(), "ToFlow", {node->Head().HeadPtr()});
            }
        }
    }
    if (node->IsCallable("FromFlow") && node->Head().IsCallable("ToFlow") && node->Head().ChildrenSize() == 1) {
        const auto input = node->Head().HeadPtr();
        const auto* type = TypeOf(*input, types);
        if (input->IsCallable({"WideFromBlocks", "WideToBlocks"}) ||
            (type && type->GetKind() == ETypeAnnotationKind::Stream)) return input;
    }
    if (node->IsCallable("ExpandMap") && node->Head().IsCallable("NarrowMap")) return UnpackRow(node, ctx);
    if (node->IsCallable("WideFromBlocks") && node->Head().IsCallable("WideToBlocks")) return node->Head().HeadPtr();
    if (node->IsCallable("WideToBlocks") && node->Head().IsCallable("WideFromBlocks")) {
        // WideFromBlocks broadcasts scalar block values; preserve that when
        // removing the round trip back to blocks.
        return ctx.NewCallable(node->Pos(), "ReplicateScalars", {node->Head().HeadPtr()});
    }
    if (!node->IsCallable("WideMap")) return node;
    const auto fields = Projection(node->Tail());
    if (fields) {
        bool identity = fields->size() == node->Tail().Head().ChildrenSize();
        for (size_t i = 0; i < fields->size(); ++i) identity &= (*fields)[i] == i;
        if (identity) return node->HeadPtr();
    }
    const auto& input = node->Head();
    if (input.IsCallable("ToFlow") && input.ChildrenSize() == 1 && input.Head().IsCallable("WideFromBlocks")) {
        if (auto projection = BlockProjection(node->Tail(), ctx, executionTypes)) {
            auto projected = ctx.NewCallable(node->Pos(), "WideMap", {input.Head().HeadPtr(), std::move(projection)});
            auto unpacked = ctx.NewCallable(node->Pos(), "WideFromBlocks", {std::move(projected)});
            return ctx.NewCallable(node->Pos(), "ToFlow", {std::move(unpacked)});
        }
    }
    if (!fields) return node;
    if (input.IsCallable("WideMap")) {
        if (const auto inner = Projection(input.Tail())) {
            TVector<size_t> composed;
            for (const auto field : *fields) composed.push_back(inner->at(field));
            return ctx.NewCallable(node->Pos(), "WideMap", {input.HeadPtr(), ProjectLambda(input.Tail(), composed, false, ctx)});
        }
    }
    return node;
}
} // namespace

NYql::TExprNode::TPtr LowerPhysicalLayouts(const NYql::TExprNode::TPtr& root, NYql::TExprContext& ctx,
    const NYql::TTypeAnnotationContext& executionTypes, const NYql::TNodeOnNodeOwnedMap& inputs)
{
    // One postorder walk of the DAG; local reductions remove adapters or
    // combine adjacent field projections, without a whole-graph fixed point.
    THashMap<const TExprNode*, TExprNode::TPtr> lowered;
    TLayoutTypes types;
    // Channel inputs change representation, while their unpacked values keep
    // the original type. Apply substitutions during the same traversal so
    // rebuilt operators retain type knowledge without a separate retyping pass.
    for (const auto& [original, replacement] : inputs) {
        lowered.emplace(original, replacement);
        if (original->GetTypeAnn()) types.emplace(replacement.Get(), original->GetTypeAnn());
    }
    TVector<std::pair<TExprNode::TPtr, bool>> pending{{root, false}};
    while (!pending.empty()) {
        auto [original, ready] = std::move(pending.back()); pending.pop_back();
        if (lowered.contains(original.Get())) continue;
        if (!ready) {
            pending.emplace_back(original, true);
            for (const auto& child : original->Children()) if (!lowered.contains(child.Get())) pending.emplace_back(child, false);
            continue;
        }
        TExprNode::TListType children;
        bool changed = false;
        for (const auto& child : original->Children()) {
            children.push_back(lowered.at(child.Get()));
            changed |= children.back() != child;
        }
        auto node = changed ? ctx.ChangeChildren(*original, std::move(children)) : original;
        for (;;) {
            auto next = Rewrite(node, ctx, types, executionTypes);
            if (next == node) break;
            node = std::move(next);
        }
        // Retain type knowledge without marking new nodes TypeComplete:
        // their freshly built lambdas still need backend annotation.
        if (node != original && original->GetTypeAnn()) types.emplace(node.Get(), original->GetTypeAnn());
        lowered.emplace(original.Get(), std::move(node));
    }
    return lowered.at(root.Get());
}

} // namespace NKikimr::NKqp
