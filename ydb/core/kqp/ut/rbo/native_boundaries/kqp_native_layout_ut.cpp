#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_layout.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_compatibility.h>

#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_type_annotation.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/minikql/mkql_function_registry.h>
#include <yql/essentials/providers/common/arrow_resolve/yql_simple_arrow_resolver.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYql;
const TPositionHandle Pos;

TExprNode::TPtr LowerAdapters(const TExprNode::TPtr& root, TExprContext& ctx) {
    TTypeAnnotationContext types;
    return LowerPhysicalLayouts(root, ctx, types);
}

TExprNode::TPtr Project(TExprContext& ctx, TExprNode::TPtr input, const TVector<size_t>& fields, size_t width) {
    TExprNode::TListType args, values;
    for (size_t i = 0; i < width; ++i) args.push_back(ctx.NewArgument(Pos, "field"));
    for (const auto i : fields) values.push_back(args.at(i));
    return ctx.NewCallable(Pos, "WideMap", {input, ctx.NewLambda(Pos, ctx.NewArguments(Pos, std::move(args)), std::move(values))});
}

TExprNode::TPtr PackAndUnpack(TExprContext& ctx, TExprNode::TPtr input, bool computed = false) {
    auto first = ctx.NewArgument(Pos, "first"), second = ctx.NewArgument(Pos, "second");
    auto name = [&](TStringBuf value) { return ctx.NewAtom(Pos, value); };
    auto value = computed ? ctx.NewCallable(Pos, "Ensure", {first, ctx.NewCallable(Pos, "Bool", {name("false")}), name("must retain")}) : first;
    auto packed = ctx.NewCallable(Pos, "AsStruct", {
        ctx.NewList(Pos, {name("a"), value}), ctx.NewList(Pos, {name("b"), second})});
    auto narrow = ctx.NewCallable(Pos, "NarrowMap", {input,
        ctx.NewLambda(Pos, ctx.NewArguments(Pos, {first, second}), std::move(packed))});
    auto row = ctx.NewArgument(Pos, "row");
    // Select/reorder/repeat fields; keep one shared wide producer.
    auto lambda = ctx.NewLambda(Pos, ctx.NewArguments(Pos, {row}), TExprNode::TListType{
        ctx.NewCallable(Pos, "Member", {row, name("b")}), ctx.NewCallable(Pos, "Member", {row, name("b")})});
    return ctx.NewCallable(Pos, "ExpandMap", {narrow, lambda});
}
}

Y_UNIT_TEST_SUITE(KqpNativePhysicalLayout) {
    Y_UNIT_TEST(ScalarBroadcastExpandsOnlySelectedPayloadsAndKeepsLength) {
        TExprContext ctx;
        TTypeAnnotationContext types;
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64);
        const auto* scalar = ctx.MakeType<TScalarExprType>(integer);
        auto blocks = ctx.NewArgument(Pos, "blocks");
        blocks->SetTypeAnn(ctx.MakeType<TStreamExprType>(ctx.MakeType<TMultiExprType>(
            TVector<const TTypeAnnotationNode*>{ctx.MakeType<TBlockExprType>(integer), scalar, scalar, scalar})));
        for (const bool selectedOnly : {false, true}) {
            auto children = TExprNode::TListType{blocks};
            if (selectedOnly) children.push_back(ctx.NewList(Pos, {ctx.NewAtom(Pos, "2")}));
            const auto macro = ctx.NewCallable(Pos, "ReplicateScalars", std::move(children));
            UNIT_ASSERT(NeedsRboCompatibilityLowering(macro));
            const auto lowered = RewriteRboCompatibilityNode(macro, ctx, types);
            UNIT_ASSERT(lowered->IsCallable("WideMap") && lowered->HeadPtr() == blocks);
            const auto& lambda = lowered->Tail();
            UNIT_ASSERT(lambda.ChildPtr(1) == lambda.Head().ChildPtr(0));
            if (selectedOnly) UNIT_ASSERT(lambda.ChildPtr(2) == lambda.Head().ChildPtr(1));
            else UNIT_ASSERT(lambda.Child(2)->IsCallable("ReplicateScalar"));
            UNIT_ASSERT(lambda.Child(3)->IsCallable("ReplicateScalar"));
            UNIT_ASSERT(lambda.Child(3)->HeadPtr() == lambda.Head().ChildPtr(2));
            UNIT_ASSERT(lambda.Child(3)->TailPtr() == lambda.Head().TailPtr());
            UNIT_ASSERT(lambda.TailPtr() == lambda.Head().TailPtr());
            EnsureRboCompatibilityLowered(lowered);
        }
    }

    Y_UNIT_TEST(IdentityRowsRequireEveryFieldWithItsOriginalName) {
        TExprContext ctx;
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Int32);
        auto row = ctx.NewArgument(Pos, "row");
        row->SetTypeAnn(ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{
            ctx.MakeType<TItemExprType>("a", integer), ctx.MakeType<TItemExprType>("b", integer)}));
        auto input = ctx.NewArgument(Pos, "input");
        auto field = [&](TStringBuf name, TStringBuf source) {
            return ctx.NewList(Pos, {ctx.NewAtom(Pos, name),
                ctx.NewCallable(Pos, "Member", {row, ctx.NewAtom(Pos, source)})});
        };
        auto map = [&](TExprNode::TListType fields) {
            return ctx.NewCallable(Pos, "Map", {input, ctx.NewLambda(Pos, ctx.NewArguments(Pos, {row}),
                ctx.NewCallable(Pos, "AsStruct", std::move(fields)))});
        };
        UNIT_ASSERT(LowerAdapters(map({field("b", "b"), field("a", "a")}), ctx) == input);
        const auto subset = map({field("a", "a")});
        UNIT_ASSERT(LowerAdapters(subset, ctx) == subset);
        const auto renamed = map({field("b", "a"), field("a", "b")});
        UNIT_ASSERT(LowerAdapters(renamed, ctx) == renamed);
    }

    Y_UNIT_TEST(BlockSortHonorsModesTypesAndPreservesKeysAndLimit) {
        TExprContext ctx;
        auto registry = NMiniKQL::CreateFunctionRegistry(NMiniKQL::CreateBuiltinRegistry());
        TTypeAnnotationContext types;
        types.ArrowResolver = MakeSimpleArrowResolver(*registry);
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Int32);
        const auto* flowType = ctx.MakeType<TFlowExprType>(ctx.MakeType<TMultiExprType>(
            TVector<const TTypeAnnotationNode*>{integer}));
        auto rows = ctx.NewArgument(Pos, "rows");
        rows->SetTypeAnn(flowType);
        auto blocks = ctx.NewArgument(Pos, "blocks");
        auto unpacked = ctx.NewCallable(Pos, "ToFlow", {ctx.NewCallable(Pos, "WideFromBlocks", {blocks})});
        unpacked->SetTypeAnn(flowType);
        auto keys = ctx.NewList(Pos, {ctx.NewList(Pos, {ctx.NewAtom(Pos, "0"),
            ctx.NewCallable(Pos, "Bool", {ctx.NewAtom(Pos, "true")})})});
        auto count = ctx.NewCallable(Pos, "Uint64", {ctx.NewAtom(Pos, "7")});
        for (const TStringBuf name : {TStringBuf("WideSort"), TStringBuf("WideTop"), TStringBuf("WideTopSort")}) {
            for (const auto& input : {rows, unpacked}) {
                auto children = TExprNode::TListType{input, keys};
                if (name != "WideSort") children.insert(children.begin() + 1, count);
                const auto sort = ctx.NewCallable(Pos, name, std::move(children));
                types.BlockEngineMode = EBlockEngineMode::Disable;
                UNIT_ASSERT(LowerPhysicalLayouts(sort, ctx, types) == sort);
                for (const auto mode : {EBlockEngineMode::Auto, EBlockEngineMode::Force}) {
                    types.BlockEngineMode = mode;
                    const auto result = LowerPhysicalLayouts(sort, ctx, types);
                    if (mode == EBlockEngineMode::Auto && input == rows) {
                        UNIT_ASSERT(result == sort);
                        continue;
                    }
                    UNIT_ASSERT(result->IsCallable("ToFlow") && result->Head().IsCallable("WideFromBlocks"));
                    const auto& blockSort = result->Head().Head();
                    UNIT_ASSERT_VALUES_EQUAL(blockSort.Content(), TString(name) + "Blocks");
                    UNIT_ASSERT(blockSort.TailPtr() == keys);
                    if (name != "WideSort") UNIT_ASSERT(blockSort.ChildPtr(1) == count);
                    UNIT_ASSERT(blockSort.Head().IsCallable(input == rows ? "WideToBlocks" : "ReplicateScalars"));
                }
            }
        }
        auto unsupported = ctx.NewArgument(Pos, "unsupported");
        unsupported->SetTypeAnn(ctx.MakeType<TFlowExprType>(ctx.MakeType<TMultiExprType>(
            TVector<const TTypeAnnotationNode*>{ctx.MakeType<TListExprType>(integer)})));
        const auto sort = ctx.NewCallable(Pos, "WideSort", {unsupported, keys});
        types.BlockEngineMode = EBlockEngineMode::Force;
        UNIT_ASSERT(LowerPhysicalLayouts(sort, ctx, types) == sort);
    }

    Y_UNIT_TEST(FieldAdaptersPreserveOrderSharingAndComputations) {
        TExprContext ctx;
        auto input = ctx.NewArgument(Pos, "input");
        const auto adapted = PackAndUnpack(ctx, input);
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Int32);
        adapted->SetTypeAnn(ctx.MakeType<TFlowExprType>(ctx.MakeType<TMultiExprType>(
            TVector<const TTypeAnnotationNode*>{integer, integer})));
        const auto root = ctx.NewList(Pos, {adapted, adapted});
        const auto result = LowerAdapters(root, ctx);
        UNIT_ASSERT(result->HeadPtr() == result->TailPtr());
        const auto& map = result->Head();
        UNIT_ASSERT(map.IsCallable("WideMap"));
        UNIT_ASSERT(!map.GetTypeAnn());
        UNIT_ASSERT(map.GetState() == TExprNode::EState::Initial);
        UNIT_ASSERT(map.HeadPtr() == input);
        UNIT_ASSERT(map.Tail().Child(1) == map.Tail().Head().Child(1));
        UNIT_ASSERT(map.Tail().Child(2) == map.Tail().Head().Child(1));
        const auto checked = PackAndUnpack(ctx, input, true);
        UNIT_ASSERT(LowerAdapters(checked, ctx) == checked);
    }

    Y_UNIT_TEST(BlockProjectionPreservesOptionalLayersAndRejectsComputations) {
        TExprContext ctx;
        auto registry = NMiniKQL::CreateFunctionRegistry(NMiniKQL::CreateBuiltinRegistry());
        TTypeAnnotationContext types;
        types.ArrowResolver = MakeSimpleArrowResolver(*registry);
        types.BlockEngineMode = EBlockEngineMode::Auto;
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64);
        auto field = ctx.NewArgument(Pos, "field");
        field->SetTypeAnn(ctx.MakeType<TOptionalExprType>(integer));
        auto literal = ctx.NewCallable(Pos, "Uint64", {ctx.NewAtom(Pos, "1")});
        literal->SetTypeAnn(integer);
        auto optional = ctx.NewCallable(Pos, "Just", {field});
        optional->SetTypeAnn(ctx.MakeType<TOptionalExprType>(field->GetTypeAnn()));
        auto blocks = ctx.NewArgument(Pos, "blocks");
        auto input = ctx.NewCallable(Pos, "ToFlow", {ctx.NewCallable(Pos, "WideFromBlocks", {blocks})});
        auto map = [&](TExprNode::TListType values) {
            return ctx.NewCallable(Pos, "WideMap", {input,
                ctx.NewLambda(Pos, ctx.NewArguments(Pos, {field}), std::move(values))});
        };
        const auto root = map({field, literal, optional, optional});
        const auto lowered = LowerPhysicalLayouts(root, ctx, types);
        UNIT_ASSERT(lowered->IsCallable("ToFlow") && lowered->Head().IsCallable("WideFromBlocks"));
        const auto& projection = lowered->Head().Head().Tail();
        UNIT_ASSERT(projection.Child(2)->IsCallable("AsScalar"));
        UNIT_ASSERT(projection.Child(2)->HeadPtr() == literal);
        UNIT_ASSERT(projection.Child(3)->IsCallable("BlockJust"));
        UNIT_ASSERT(projection.Child(3)->HeadPtr() == projection.Head().HeadPtr());
        UNIT_ASSERT(projection.ChildPtr(3) == projection.ChildPtr(4));
        UNIT_ASSERT(projection.TailPtr() == projection.Head().TailPtr());
        types.BlockEngineMode = EBlockEngineMode::Disable;
        UNIT_ASSERT(LowerPhysicalLayouts(root, ctx, types) == root);
        types.BlockEngineMode = EBlockEngineMode::Auto;
        const auto volatileMap = map({ctx.NewCallable(Pos, "Random", {field})});
        UNIT_ASSERT(LowerPhysicalLayouts(volatileMap, ctx, types) == volatileMap);
        const auto checkedMap = map({ctx.NewCallable(Pos, "Ensure", {field,
            ctx.NewCallable(Pos, "Bool", {ctx.NewAtom(Pos, "false")}), ctx.NewAtom(Pos, "must retain")})});
        UNIT_ASSERT(LowerPhysicalLayouts(checkedMap, ctx, types) == checkedMap);
    }

    Y_UNIT_TEST(ChannelSubstitutionRetainsLogicalTypesUntilBackendAnnotation) {
        TExprContext ctx;
        auto registry = NMiniKQL::CreateFunctionRegistry(NMiniKQL::CreateBuiltinRegistry());
        TTypeAnnotationContext types;
        types.ArrowResolver = MakeSimpleArrowResolver(*registry);
        types.BlockEngineMode = EBlockEngineMode::Auto;
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64);
        const auto* row = ctx.MakeType<TMultiExprType>(TVector<const TTypeAnnotationNode*>{integer});
        auto input = ctx.NewArgument(Pos, "input");
        input->SetTypeAnn(ctx.MakeType<TStreamExprType>(row));
        auto flow = ctx.NewCallable(Pos, "ToFlow", {input});
        flow->SetTypeAnn(ctx.MakeType<TFlowExprType>(row));
        auto keys = ctx.NewList(Pos, {ctx.NewList(Pos, {ctx.NewAtom(Pos, "0"),
            ctx.NewCallable(Pos, "Bool", {ctx.NewAtom(Pos, "true")})})});
        auto sort = ctx.NewCallable(Pos, "WideSort", {flow, keys});
        auto blockInput = ctx.NewArgument(Pos, "blocks");
        TNodeOnNodeOwnedMap replacements{{input.Get(), ctx.NewCallable(Pos, "WideFromBlocks", {blockInput})}};
        const auto result = LowerPhysicalLayouts(sort, ctx, types, replacements);
        UNIT_ASSERT(result->IsCallable("ToFlow") && result->Head().IsCallable("WideFromBlocks"));
        UNIT_ASSERT(result->Head().Head().IsCallable("WideSortBlocks"));
        UNIT_ASSERT(!result->GetTypeAnn());
    }

    Y_UNIT_TEST(ProjectionCompositionDoesNotCopyProducerOrReorderValues) {
        TExprContext ctx;
        auto input = ctx.NewArgument(Pos, "input");
        const auto composed = Project(ctx, Project(ctx, input, {2, 0, 1}, 3), {1, 2, 0}, 3);
        UNIT_ASSERT(LowerAdapters(composed, ctx) == input);
        const auto selected = LowerAdapters(Project(ctx, Project(ctx, input, {2, 0, 1}, 3), {0, 0}, 3), ctx);
        UNIT_ASSERT(selected->HeadPtr() == input);
        UNIT_ASSERT(selected->Tail().Child(1) == selected->Tail().Head().Child(2));
        UNIT_ASSERT(selected->Tail().Child(2) == selected->Tail().Head().Child(2));
    }

    Y_UNIT_TEST(BlockProjectionsRetainLengthAndScalarBroadcasting) {
        TExprContext ctx;
        auto blocks = ctx.NewArgument(Pos, "blocks");
        auto unpack = ctx.NewCallable(Pos, "WideFromBlocks", {blocks});
        auto flow = ctx.NewCallable(Pos, "ToFlow", {unpack});
        auto project = Project(ctx, flow, {1, 0, 1}, 2);
        auto stream = ctx.NewCallable(Pos, "FromFlow", {project});
        const auto result = LowerAdapters(stream, ctx);
        UNIT_ASSERT(result->IsCallable("WideFromBlocks"));
        const auto& map = result->Head();
        UNIT_ASSERT(map.IsCallable("WideMap"));
        UNIT_ASSERT(map.HeadPtr() == blocks);
        UNIT_ASSERT_VALUES_EQUAL(map.Tail().Head().ChildrenSize(), 3);
        UNIT_ASSERT_VALUES_EQUAL(map.Tail().ChildrenSize(), 5);
        UNIT_ASSERT(map.Tail().Child(1) == map.Tail().Head().Child(1));
        UNIT_ASSERT(map.Tail().Child(2) == map.Tail().Head().Child(0));
        UNIT_ASSERT(map.Tail().Child(3) == map.Tail().Head().Child(1));
        UNIT_ASSERT(map.Tail().TailPtr() == map.Tail().Head().TailPtr());
        const auto repacked = LowerAdapters(ctx.NewCallable(Pos, "WideToBlocks", {unpack}), ctx);
        UNIT_ASSERT(repacked->IsCallable("ReplicateScalars"));
        UNIT_ASSERT(repacked->HeadPtr() == blocks);
    }

    Y_UNIT_TEST(StreamAndListAdaptersKeepTheirResultKinds) {
        TExprContext ctx;
        auto input = ctx.NewArgument(Pos, "input");
        input->SetTypeAnn(ctx.MakeType<TListExprType>(ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Int32)));
        auto flow = ctx.NewCallable(Pos, "ToFlow", {input});
        auto stream = ctx.NewCallable(Pos, "FromFlow", {flow});
        // A list is not a stream, even when they have identical item types.
        UNIT_ASSERT(LowerAdapters(stream, ctx) == stream);
        UNIT_ASSERT(LowerAdapters(ctx.NewCallable(Pos, "ToFlow", {stream}), ctx) == flow);
        const auto iterator = ctx.NewCallable(Pos, "Iterator", {input});
        const auto lowered = LowerAdapters(ctx.NewCallable(Pos, "ToFlow", {iterator}), ctx);
        UNIT_ASSERT(lowered->IsCallable("ToFlow"));
        UNIT_ASSERT(lowered->HeadPtr() == input);
    }

    Y_UNIT_TEST(AdaptersPreserveDependenciesAndOptionalListIteration) {
        TExprContext ctx;
        const auto* integer = ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Int32);
        const auto* listType = ctx.MakeType<TListExprType>(integer);
        auto list = ctx.NewArgument(Pos, "list");
        list->SetTypeAnn(listType);
        auto dependency = ctx.NewCallable(Pos, "DependsOn", {ctx.NewArgument(Pos, "scope")});
        const auto iterator = ctx.NewCallable(Pos, "Iterator", {list, dependency});
        const auto dependentIterator = ctx.NewCallable(Pos, "ToFlow", {iterator});
        UNIT_ASSERT(LowerAdapters(dependentIterator, ctx) == dependentIterator);

        auto stream = ctx.NewArgument(Pos, "stream");
        stream->SetTypeAnn(ctx.MakeType<TStreamExprType>(integer));
        const auto dependentFlow = ctx.NewCallable(Pos, "ToFlow", {stream, dependency});
        const auto fromFlow = ctx.NewCallable(Pos, "FromFlow", {dependentFlow});
        UNIT_ASSERT(LowerAdapters(fromFlow, ctx) == fromFlow);
        const auto toFlow = ctx.NewCallable(Pos, "ToFlow", {ctx.NewCallable(Pos, "FromFlow", {dependentFlow}), dependency});
        UNIT_ASSERT(LowerAdapters(toFlow, ctx) == toFlow);

        auto optionalList = ctx.NewArgument(Pos, "optional_list");
        optionalList->SetTypeAnn(ctx.MakeType<TOptionalExprType>(listType));
        const auto optionalIterator = ctx.NewCallable(Pos, "ToFlow", {ctx.NewCallable(Pos, "Iterator", {optionalList})});
        UNIT_ASSERT(LowerAdapters(optionalIterator, ctx) == optionalIterator);

        auto blocks = ctx.NewArgument(Pos, "blocks");
        auto blockFlow = ctx.NewCallable(Pos, "ToFlow", {ctx.NewCallable(Pos, "WideFromBlocks", {blocks}), dependency});
        auto selected = Project(ctx, blockFlow, {1, 0}, 2);
        UNIT_ASSERT(LowerAdapters(selected, ctx) == selected);
    }
}
} // namespace NKikimr::NKqp
