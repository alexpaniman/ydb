#include "kqp_rbo_test_helpers.h"

#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_aggregation_builder.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_table_effect_builder.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_convertion_utils.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_union_all_builder.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_source_builder.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_lookup_join_builder.h>
#include <ydb/core/kqp/opt/rbo/kqp_olap_expr_inspection.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_transformer.h>

#include <yql/essentials/core/yql_expr_optimize.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {
namespace {
TNativeTable NoTables(std::string_view, NFrontend::TSourcePosition pos) {
    throw NFrontend::TFrontendError(pos, "unexpected table lookup");
}
}


Y_UNIT_TEST_SUITE(KqpRboIdLowering) {
    Y_UNIT_TEST(UpsertKeepsDefaultColumnsThroughLowering) {
        NTests::TIdTestContext f;
        const auto value = f.Id("value");
        const auto nodes = f.ExprCtx.NodeAllocationCounter;
        const auto types = f.ExprCtx.TypeNodes.size();
        const TRboTable table("/Root/table", "1:1", "", "1");
        TEffectOptions options;
        options.DefaultColumns = TVector<TString>{"value"};
        TOpTableEffect effect(f.Read({value}), f.Pos, table, EEffectType::UpsertRows, options, {{value, "value"}}, {});
        UNIT_ASSERT_VALUES_EQUAL(effect.GetExplainName(), "UpsertRows");
        const auto stage = f.Props.StageGraph.AddSinkStage({effect.Table, effect.EffectType, effect.Options});
        UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
        UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
        const NNodes::TKqpTableSinkSettings settings(LowerTableSinkSettings(f.Props.StageGraph.GetSinkSettings(stage), f.ExprCtx, f.Pos));
        UNIT_ASSERT_VALUES_EQUAL(settings.DefaultColumns().Size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(settings.DefaultColumns().Item(0).StringValue(), "value");
    }

    Y_UNIT_TEST(TypeAnnotationAllowsSharingOnlyThroughDistinctReplicatePorts) {
        // Extra local references are harmless. Reusing an ordinary subtree or
        // one output port in two plan slots violates the binding invariant.
        enum class EShape {
            SameSubtree,
            DistinctPorts,
            SamePort,
            PortAndNakedSource,
            PortsOfTwoReplicates,
            NestedDistinctPorts,
        };
        for (const bool packed : {false, true}) {
            for (const auto shape : {EShape::SameSubtree, EShape::DistinctPorts, EShape::SamePort,
                     EShape::PortAndNakedSource, EShape::PortsOfTwoReplicates, EShape::NestedDistinctPorts}) {
                NTests::TIdTestContext f;
                auto source = MakeIntrusive<TOpEmptySource>(f.Pos);
                auto hub = TReplicate::Create(source, f.Pos, f.Props.InfoUnitRegistry);
                auto port = hub->AddOutput();
                TIntrusivePtr<IOperator> left = source, right = source;
                switch (shape) {
                    case EShape::SameSubtree:
                        break;
                    case EShape::DistinctPorts:
                        left = port;
                        right = hub->AddOutput();
                        break;
                    case EShape::SamePort:
                        left = right = port;
                        break;
                    case EShape::PortAndNakedSource:
                        left = port; // A naked use may not bypass the binding.
                        break;
                    case EShape::PortsOfTwoReplicates:
                        left = port;
                        right = TReplicate::Create(source, f.Pos, f.Props.InfoUnitRegistry)->AddOutput();
                        break;
                    case EShape::NestedDistinctPorts: {
                        auto nested = TReplicate::Create(port, f.Pos, f.Props.InfoUnitRegistry);
                        left = nested->AddOutput();
                        right = nested->AddOutput();
                        break;
                    }
                }
                TIntrusivePtr<IOperator> plan = MakeIntrusive<TOpJoin>(left, right, f.Pos, "Cross", TJoinIUs{});
                if (packed) {
                    plan = MakeIntrusive<TOpCBOTree>(plan, f.Pos);
                }
                auto root = f.Root(plan, {});
                const auto status = root->ComputeTypes(f.RboCtx);
                const bool valid = shape == EShape::DistinctPorts || shape == EShape::NestedDistinctPorts;
                UNIT_ASSERT_C(status == (valid ? IGraphTransformer::TStatus::Ok : IGraphTransformer::TStatus::Error),
                    f.ExprCtx.IssueManager.GetIssues().ToString());
            }
        }

        // Two subplan bindings, or a subplan and the main plan, must not own
        // the same ordinary subtree either.
        for (const bool shareWithMain : {false, true}) {
            NTests::TIdTestContext f;
            const auto callA = f.Id(), callB = f.Id();
            auto source = MakeIntrusive<TOpEmptySource>(f.Pos);
            f.Props.Subplans.Add(callA, source, ESubplanType::EXISTS);
            f.Props.Subplans.Add(callB, source, ESubplanType::EXISTS);
            TIntrusivePtr<IOperator> plan = shareWithMain ? source : MakeIntrusive<TOpEmptySource>(f.Pos);
            plan = MakeIntrusive<TOpFilter>(plan, f.Pos, f.Column(callA));
            if (!shareWithMain) {
                plan = MakeIntrusive<TOpFilter>(plan, f.Pos, f.Column(callB));
            }
            auto root = f.Root(plan, {});
            UNIT_ASSERT(root->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Error);
            UNIT_ASSERT_C(f.ExprCtx.IssueManager.GetIssues().ToString().Contains("sharing requires distinct ports of the same Replicate"),
                f.ExprCtx.IssueManager.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(NativeTypeCheckingNeverCallsTheBackendAnnotator) {
        NTests::TIdTestContext f;
        const auto result = f.Id("before");
        TMapIUs definitions;
        definitions.Add(result, f.Constant());
        auto root = f.Root(MakeIntrusive<TOpMap>(MakeIntrusive<TOpEmptySource>(f.Pos),
            f.Pos, std::move(definitions)), {{result, "result"}});

        auto transformer = CreateFunctorTransformer([](TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) {
            output = input;
            ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), "Original YQL leaf diagnostic"));
            return IGraphTransformer::TStatus::Error;
        });
        TRBOContext ctx(f.KqpCtx, f.ExprCtx, f.TypeCtx, *transformer, *f.FuncRegistry);
        UNIT_ASSERT(root->ComputeTypes(ctx) == IGraphTransformer::TStatus::Ok);
        UNIT_ASSERT(f.ExprCtx.IssueManager.GetIssues().Empty());
        UNIT_ASSERT(root->GetInput()->GetNativeIUType(result) == NFrontend::TType{NFrontend::ETypeKind::Uint64});
    }

    void CheckSubquerySharing(bool secondaryFirst, bool correlated) {
        NTests::TIdTestContext f;
        const auto value = f.Id("payload"), outerValue = f.Id("outer");
        TMapIUs values;
        values.Add(value, f.Constant());
        auto producer = MakeIntrusive<TOpMap>(MakeIntrusive<TOpEmptySource>(f.Pos), f.Pos, values);
        auto hub = TReplicate::Create(producer, f.Pos, f.Props.InfoUnitRegistry);
        auto first = hub->AddOutput(), second = hub->AddOutput();
        if (secondaryFirst) std::swap(first, second);
        TUnorderedIUs captures;
        TMapIUs calls;
        for (auto port : {first, second}) {
            TIntrusivePtr<IOperator> plan = port;
            if (correlated) {
                const auto local = f.Id("capture");
                captures.Add(local);
                plan = MakeIntrusive<TOpAddDependencies>(plan, f.Pos,
                    TDependencyIUs{{local, TCapturedIU{outerValue, NFrontend::TType{NFrontend::ETypeKind::Uint64}}}});
            }
            const auto result = port->IsPrimary() ? value : port->GetRebindings().At(value);
            const auto call = f.Id("call"), output = f.Id("output");
            f.Props.Subplans.Add(call, plan, ESubplanType::EXPR, {}, result);
            calls.Add(output, f.Column(call));
            UNIT_ASSERT(plan->GetOutputIUs().Contains(result));
        }
        TMapIUs outerValues;
        outerValues.Add(outerValue, f.Constant());
        auto outer = MakeIntrusive<TOpMap>(MakeIntrusive<TOpEmptySource>(f.Pos), f.Pos, std::move(outerValues));
        auto root = f.Root(MakeIntrusive<TOpMap>(outer, f.Pos, calls), {});
        UNIT_ASSERT(root->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
        UNIT_ASSERT_VALUES_EQUAL(captures.Size(), correlated ? 2 : 0);
        UNIT_ASSERT(&first->GetReplicate() == &second->GetReplicate());
        UNIT_ASSERT(!first->GetOutputIUs().HasAny(second->GetOutputIUs()));
    }

    Y_UNIT_TEST(CaptureFreeSubtreesShareTheirProducerAcrossCallSites) {
        for (const bool sharedCallNode : {false, true}) {
            for (const bool correlated : {false, true}) {
                CheckSubquerySharing(sharedCallNode, correlated);
            }
        }
    }

    Y_UNIT_TEST(ScalarReferencesDoNotCreateReplicateConsumers) {
        NTests::TIdTestContext f;
        auto query = CompileNativeToRbo("SELECT 1 AS payload ORDER BY payload", NoTables, f.ExprCtx);
        size_t sorts = 0;
        for (const auto& item : *query.Roots.front()) {
            UNIT_ASSERT(item.Current->Kind != EOperator::Replicate);
            sorts += item.Current->Kind == EOperator::Sort;
        }
        UNIT_ASSERT_VALUES_EQUAL(sorts, 1);
        UNIT_ASSERT(query.Roots.front()->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
    }

    Y_UNIT_TEST(RepeatedPlanInputCreatesDisjointReplicatePorts) {
        NTests::TIdTestContext f;
        auto query = CompileNativeToRbo("$q = (SELECT 1 AS payload); SELECT * FROM $q UNION ALL SELECT * FROM $q;", NoTables, f.ExprCtx);
        TOpUnionAll* unionOp = nullptr;
        for (const auto& item : *query.Roots.front()) if (item.Current->Kind == EOperator::UnionAll) unionOp = &CastOperator<TOpUnionAll>(*item.Current);
        UNIT_ASSERT(unionOp);
        auto& merge = *unionOp;
        auto port = [](IOperator* op) -> TOpReplicate& {
            while (op->Kind != EOperator::Replicate) { UNIT_ASSERT_VALUES_EQUAL(op->GetChildCount(), 1); op = op->GetChild(0).Get(); }
            return CastOperator<TOpReplicate>(*op);
        };
        auto& left = port(merge.GetChild(0).Get());
        auto& right = port(merge.GetChild(1).Get());
        UNIT_ASSERT(&left.GetReplicate() == &right.GetReplicate());
        UNIT_ASSERT(left.IsPrimary());
        UNIT_ASSERT(!right.IsPrimary());
        UNIT_ASSERT(!left.GetOutputIUs().HasAny(right.GetOutputIUs()));
        const auto output = *merge.GetColumns().Keys().begin();
        const auto& inputs = merge.GetColumns().Find(output)->Inputs;
        UNIT_ASSERT(merge.GetChild(0)->GetOutputIUs().Contains(inputs[0]));
        UNIT_ASSERT(merge.GetChild(1)->GetOutputIUs().Contains(inputs[1]));
        UNIT_ASSERT(output != inputs[0] && output != inputs[1]);
        UNIT_ASSERT(left.GetReplicate().GetInput()->GetChild(0)->Kind == EOperator::EmptySource);
    }

    Y_UNIT_TEST(PositionalRenamePreservesProjectionAcrossSharedInputs) {
        for (const auto kind : {"UNION ALL", "UNION"}) {
            NTests::TIdTestContext f;
            const std::string sql = std::string("$q = (SELECT 1 AS z, 2 AS a); SELECT z AS first, a AS second FROM $q ")
                + kind + " SELECT z AS first, a AS second FROM $q;";
            auto query = CompileNativeToRbo(sql, NoTables, f.ExprCtx);
            auto& root = *query.Roots.front();
            UNIT_ASSERT_VALUES_EQUAL(root.GetColumns().Items()[0].second, "first");
            UNIT_ASSERT_VALUES_EQUAL(root.GetColumns().Items()[1].second, "second");
            UNIT_ASSERT(root.ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
            NTests::AssertIdInvariants(*root.GetInput(), root.PlanProps);
            for (const auto& item : root) if (item.Current->Kind == EOperator::UnionAll) {
                auto& merge = CastOperator<TOpUnionAll>(*item.Current);
                UNIT_ASSERT(!merge.GetChild(0)->GetOutputIUs().HasAny(merge.GetChild(1)->GetOutputIUs()));
                for (const auto& [output, columns] : merge.GetColumns().Items()) {
                    Y_UNUSED(output);
                    for (size_t i = 0; i < columns.Inputs.size(); ++i) UNIT_ASSERT(merge.GetChild(i)->GetOutputIUs().Contains(columns.Inputs[i]));
                }
            }
        }
    }

    Y_UNIT_TEST(RootKeepsOptionalQueryHintsSeparateFromOutputNames) {
        for (const bool withHints : {false, true}) {
            NTests::TIdTestContext f;
            const auto payload = f.Id("payload");
            TOpRoot root(f.Read({payload}), f.Pos, {{payload, "payload"}}, withHints ? TVector<TString>{"display"} : TVector<TString>{});
            UNIT_ASSERT_VALUES_EQUAL(root.GetColumns().Items().front().second, "payload");
            UNIT_ASSERT_VALUES_EQUAL(root.GetQueryColumns().size(), withHints ? 1 : 0);
            if (withHints) UNIT_ASSERT_VALUES_EQUAL(root.GetQueryColumns().front(), "display");
        }
    }

    Y_UNIT_TEST(WideExpressionPreservesNestedFieldsAndLambdaArguments) {
        TExprContext ctx;
        const TPositionHandle pos;
        auto row = ctx.NewArgument(pos, "row");
        auto local = ctx.NewArgument(pos, "local");
        auto field = ctx.NewCallable(pos, "Member", {row, ctx.NewAtom(pos, "7")});
        auto nestedField = ctx.NewCallable(pos, "Member", {field, ctx.NewAtom(pos, "user_field")});
        auto localField = ctx.NewCallable(pos, "Member", {local, ctx.NewAtom(pos, "7")});
        auto nestedBody = ctx.NewList(pos, {field, localField});
        auto nestedLambda = ctx.NewLambda(pos, ctx.NewArguments(pos, {local}), std::move(nestedBody));
        auto body = ctx.NewList(pos, {nestedField, nestedLambda});
        auto lambda = ctx.NewLambda(pos, ctx.NewArguments(pos, {row}), std::move(body));
        auto wide = ctx.NewArgument(pos, "wide");

        const auto lowered = NPhysicalConvertionUtils::LowerRowLambdaBody(lambda, {{7, wide}}, ctx);
        UNIT_ASSERT(lowered->Child(0)->HeadPtr() == wide);
        UNIT_ASSERT_VALUES_EQUAL(lowered->Child(0)->Tail().Content(), "user_field");
        UNIT_ASSERT(lowered->Child(1)->Tail().ChildPtr(0) == wide);
        const auto& inner = *lowered->Child(1);
        const auto& untouched = *inner.Tail().Child(1);
        UNIT_ASSERT(&untouched.Head() == &inner.Head().Head());
        UNIT_ASSERT_VALUES_EQUAL(untouched.Tail().Content(), "7");
    }

    Y_UNIT_TEST(JoinFilterSkipsPhysicalKeyTemporaries) {
        TExprContext ctx;
        const TPositionHandle pos;
        TPlanProps props;
        props.NativeExpressions = std::make_shared<NFrontend::TExpressionArena>();
        auto left = MakeColumnAccess(7, NFrontend::TType{NFrontend::ETypeKind::Bool}, pos, &props);
        auto right = MakeColumnAccess(8, NFrontend::TType{NFrontend::ETypeKind::Bool, true}, pos, &props);
        auto filter = MakeConjunction({left, right});
        // Slots 1 and 3 are cast keys, not logical bindings.
        const auto lowered = NPhysicalConvertionUtils::ConvertToWideJoinFilter(
            LowerExpressionLambda(filter, ctx), {{7, 0}, {8, 2}}, TUnorderedIUs{8}, 4, ctx);
        UNIT_ASSERT_VALUES_EQUAL(lowered->Head().ChildrenSize(), 4);
        const auto& conjunction = lowered->Tail().Head(); // Coalesce(And(...), false).
        UNIT_ASSERT(conjunction.IsCallable("And"));
        UNIT_ASSERT(&conjunction.Head() == lowered->Head().Child(0));
        UNIT_ASSERT(conjunction.Tail().IsCallable("Unwrap"));
        UNIT_ASSERT(&conjunction.Tail().Head() == lowered->Head().Child(2));
    }

    Y_UNIT_TEST(OlapExportRenamesBindingsNotLiteralsOrNestedFields) {
        TExprContext ctx;
        const TPositionHandle pos;
        auto atom = [&](TStringBuf value) { return ctx.NewAtom(pos, value); };
        auto nested = ctx.NewCallable(pos, "StructType", {
            ctx.NewList(pos, {atom("0"), ctx.NewCallable(pos, "DataType", {atom("Utf8")})})});
        auto rowType = ctx.NewCallable(pos, "StructType", {ctx.NewList(pos, {atom("0"), nested})});
        auto column = ctx.NewCallable(pos, "KqpOlapApplyColumnArg", {rowType, atom("0")});
        auto value = ctx.NewArgument(pos, "value");
        auto kernel = ctx.NewLambda(pos, ctx.NewArguments(pos, {value}),
            ctx.NewCallable(pos, "Member", {value, atom("0")}));
        auto apply = ctx.NewCallable(pos, "KqpOlapApply", {kernel, ctx.NewList(pos, {column}), atom("0")});
        auto projection = ctx.NewCallable(pos, "KqpOlapProjection", {apply, atom("0")});
        auto row = ctx.NewArgument(pos, "row");
        auto literal = ctx.NewCallable(pos, "Utf8", {atom("0")});
        auto condition = ctx.NewList(pos, {atom("eq"), atom("0"), literal});
        auto filter = ctx.NewCallable(pos, "KqpOlapFilter", {row, condition});
        auto body = ctx.NewCallable(pos, "KqpOlapProjections", {filter, ctx.NewList(pos, {projection})});
        auto lambda = ctx.NewLambda(pos, ctx.NewArguments(pos, {row}), std::move(body));
        auto exported = NOpt::TOlapFilterInspector::RenameColumns(lambda, {{"0", "payload"}}, ctx);

        const auto& renamedCondition = exported->Tail().Head().Tail();
        UNIT_ASSERT_VALUES_EQUAL(renamedCondition.Child(1)->Content(), "payload");
        UNIT_ASSERT(renamedCondition.ChildPtr(2) == literal);
        const auto& renamedProjection = exported->Tail().Tail().Head();
        UNIT_ASSERT_VALUES_EQUAL(renamedProjection.Tail().Content(), "payload");
        const auto& renamedApply = renamedProjection.Head();
        UNIT_ASSERT(renamedApply.HeadPtr() == kernel);
        UNIT_ASSERT_VALUES_EQUAL(renamedApply.Tail().Content(), "0"); // Kernel name.
        const auto& renamedColumn = renamedApply.Child(1)->Head();
        UNIT_ASSERT_VALUES_EQUAL(renamedColumn.Tail().Content(), "payload");
        UNIT_ASSERT_VALUES_EQUAL(renamedColumn.Head().Head().Head().Content(), "payload");
        UNIT_ASSERT(renamedColumn.Head().Head().TailPtr() == nested);
        UNIT_ASSERT_VALUES_EQUAL(projection->Tail().Content(), "0"); // Original IR untouched.
    }

    Y_UNIT_TEST(UnionAllPreservesBindingsOrderAndEmptyRows) {
        NTests::TIdTestContext f;
        const auto a = f.Id(), b = f.Id(), first = f.Id(), second = f.Id();
        const TPhysicalNames names(f.Props.InfoUnitRegistry);
        for (const bool ordered : {false, true}) {
            for (const bool empty : {false, true}) {
                auto merge = f.Union(f.Read({a}), f.Read({b}),
                    {{first, a, b}, {second, a, b}}, ordered);
                const auto outputs = empty ? TUnorderedIUs{} : TUnorderedIUs{first, second};
                merge->PruneOutputs(outputs, f.ExprCtx);
                merge->Props.Analysis.LiveOut = outputs;
                merge->Props.Analysis.LiveInByChild = empty
                    ? TVector<TUnorderedIUs>(2) : TVector<TUnorderedIUs>{{a}, {b}};
                const auto ast = TPhysicalUnionAllBuilder(*merge, f.ExprCtx, f.Pos, names).BuildPhysicalOp(
                    {f.ExprCtx.NewArgument(f.Pos, "left"), f.ExprCtx.NewArgument(f.Pos, "right")});
                UNIT_ASSERT(ast->IsCallable(ordered ? "OrderedExtend" : "Extend"));
                UNIT_ASSERT_VALUES_EQUAL(ast->ChildrenSize(), 2);
                for (ui32 child = 0; child < 2; ++child) {
                    const auto& map = ast->Child(child);
                    UNIT_ASSERT(map->IsCallable(ordered ? "OrderedMap" : "Map"));
                    const auto& fields = map->Tail().Tail();
                    UNIT_ASSERT(fields.IsCallable("AsStruct"));
                    UNIT_ASSERT_VALUES_EQUAL(fields.ChildrenSize(), empty ? 0 : 2);
                    if (!empty) {
                        for (ui32 index = 0; index < 2; ++index) {
                            const auto& field = *fields.Child(index);
                            UNIT_ASSERT_VALUES_EQUAL(field.Head().Content(), names.Get(index ? second : first));
                            UNIT_ASSERT_VALUES_EQUAL(field.Tail().Tail().Content(), names.Get(child ? b : a));
                        }
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(ReadSeparatesStorageNamesFromPhysicalIds) {
        TExprContext ctx;
        const TPositionHandle pos;
        TInfoUnitRegistry registry;
        const auto z = registry.Add(TInfoUnit("t", "z"));
        const auto a = registry.Add(TInfoUnit("t", "a"));
        const auto anotherZ = registry.Add(TInfoUnit("other", "z"));
        const TPhysicalNames names(registry);
        const TRboTable table{"/Root/table", "1:1", "", "1"};
        auto arena = std::make_shared<NFrontend::TExpressionArena>();
        for (const auto storage : {NYql::EStorageType::RowStorage, NYql::EStorageType::ColumnStorage}) {
            TOpRead read("t", {z, a, anotherZ}, storage, table, nullptr, std::nullopt,
                std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, pos);
            if (storage == NYql::EStorageType::ColumnStorage) {
                const auto value = arena->Column(z, {NFrontend::ETypeKind::Uint64});
                const auto zero = arena->Literal({NFrontend::ETypeKind::Uint64}, std::uint64_t{0});
                read.OlapProgram = TOlapProgram(TExpression::FromNative(arena, arena->Binary(NFrontend::EOpcode::Greater, value, zero)));
            }
            const auto result = TPhysicalSourceBuilder(read, ctx, pos, names, registry, "stage").BuildPhysicalOp();
            TExprNode::TPtr storageRead;
            VisitExpr(result, [&](const TExprNode::TPtr& node) {
                if (TKqpReadRangesSourceSettings::Match(node.Get()) || TKqpBlockReadOlapTableRanges::Match(node.Get())) {
                    storageRead = node;
                }
                return true;
            });
            UNIT_ASSERT(storageRead);
            if (storage == NYql::EStorageType::ColumnStorage) {
                const auto process = TKqpBlockReadOlapTableRanges(storageRead).Process();
                UNIT_ASSERT(!process.Args().Arg(0).Ref().GetTypeAnn());
                UNIT_ASSERT(read.OlapProgram.Native().Predicate->GetRawInputIUs() == TUnorderedIUs{z});
                UNIT_ASSERT(FindNode(process.Body().Ptr(), [](const TExprNode::TPtr& node) { return node->IsAtom("z"); }));
            }
            const auto columns = storage == NYql::EStorageType::RowStorage
                ? TKqpReadRangesSourceSettings(storageRead).Columns()
                : TKqpBlockReadOlapTableRanges(storageRead).Columns();
            UNIT_ASSERT_VALUES_EQUAL(columns.Size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(columns.Item(0).Value(), "a");
            UNIT_ASSERT_VALUES_EQUAL(columns.Item(1).Value(), "z");

            const auto map = storage == NYql::EStorageType::RowStorage
                ? TDqPhyStage(result).Program().Body().Ptr() : result->HeadPtr();
            const auto& lambda = map->Tail();
            const auto& fields = lambda.Tail();
            UNIT_ASSERT_VALUES_EQUAL(fields.ChildrenSize(), 3);
            UNIT_ASSERT_VALUES_EQUAL(fields.Child(0)->Head().Content(), names.Get(z));
            UNIT_ASSERT_VALUES_EQUAL(fields.Child(1)->Head().Content(), names.Get(a));
            UNIT_ASSERT_VALUES_EQUAL(fields.Child(2)->Head().Content(), names.Get(anotherZ));
            if (storage == NYql::EStorageType::ColumnStorage) {
                UNIT_ASSERT(&fields.Child(0)->Tail() == lambda.Head().Child(1));
                UNIT_ASSERT(&fields.Child(1)->Tail() == lambda.Head().Child(0));
                UNIT_ASSERT(&fields.Child(2)->Tail() == lambda.Head().Child(1));
            }
        }
    }

    Y_UNIT_TEST(EmptyOlapReadUsesOnlyAPhysicalCarrier) {
        NTests::TIdTestContext f;
        const auto table = f.Read({})->TableRef;
        TOpRead read("t", {}, NYql::EStorageType::ColumnStorage, table, nullptr, std::nullopt,
            std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, f.Pos);
        const TPhysicalNames names(f.Props.InfoUnitRegistry);
        const auto ast = TPhysicalSourceBuilder(read, f.ExprCtx, f.Pos, names,
            f.Props.InfoUnitRegistry, "stage", /*isSysView=*/false, "key").BuildPhysicalOp();
        const auto source = FindNode(ast, [](const auto& node) {
            return TKqpBlockReadOlapTableRanges::Match(node.Get());
        });
        const auto columns = TKqpBlockReadOlapTableRanges(source).Columns();
        UNIT_ASSERT_VALUES_EQUAL(columns.Size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(columns.Item(0).Value(), "key");
        const auto& narrow = ast->Head();
        UNIT_ASSERT(narrow.IsCallable("NarrowMap"));
        UNIT_ASSERT_VALUES_EQUAL(narrow.Tail().Head().ChildrenSize(), 1);
        UNIT_ASSERT(narrow.Tail().Tail().IsCallable("AsStruct"));
        UNIT_ASSERT_VALUES_EQUAL(narrow.Tail().Tail().ChildrenSize(), 0);
        UNIT_ASSERT(read.GetOutputIUs().Empty());
    }

    Y_UNIT_TEST(AggregationCollapsesRepeatedKeysButPreservesDistinctOutputBindings) {
        for (const bool distinct : {false, true}) {
            for (const bool prune : {false, true}) {
                NTests::TIdTestContext f;
                const auto a = f.Id(), b = f.Id(), x = f.Id(), y = f.Id(), z = f.Id();
                TAggregationIUs traits;
                TOrderedIUs<TString> outputs{{a, "a"}, {b, "b"}};
                if (distinct) {
                    traits.Add(x, TOpAggregationTraits{a, "distinct"});
                    traits.Add(y, TOpAggregationTraits{a, "distinct"});
                    traits.Add(z, TOpAggregationTraits{b, "distinct"});
                    outputs = {{x, "x"}, {y, "y"}, {z, "z"}};
                }
                auto aggregate = MakeIntrusive<TOpAggregate>(f.Read({a, b}), std::move(traits),
                    TOrderedIUs<>{a, a, b}, EOpPhase::Intermediate, distinct, f.Pos);
                f.SetType(*aggregate->GetInput());
                f.SetType(*aggregate);
                auto* op = aggregate.get();
                auto root = f.Root(std::move(aggregate), std::move(outputs));
                ComputePlanLiveness(*root);
                const TPhysicalNames names(root->PlanProps.InfoUnitRegistry);
                const auto ast = TPhysicalAggregationBuilder(*op, f.ExprCtx, f.Pos, names, prune)
                    .BuildPhysicalOp(f.ExprCtx.NewArgument(f.Pos, "input"), std::nullopt);
                const auto combiner = FindNode(ast, [](const auto& node) {
                    return node->IsCallable("DqPhyHashCombine");
                });
                UNIT_ASSERT(combiner);
                UNIT_ASSERT_VALUES_EQUAL(combiner->Child(2)->ChildrenSize(), 3); // Args + two unique keys.
                const auto& finish = *combiner->Child(5);
                UNIT_ASSERT_VALUES_EQUAL(finish.ChildrenSize(), distinct ? 4 : 3);
                if (distinct) {
                    UNIT_ASSERT(finish.Child(1) == finish.Child(2));
                    UNIT_ASSERT(finish.Child(1) != finish.Child(3));
                }
            }
        }
    }

    Y_UNIT_TEST(ReplicateRebindsSparsePortsBeforeConnectionsAndLookupOnlyChangesItsBranch) {
        TExprContext ctx;
        const TPositionHandle pos;
        TInfoUnitRegistry registry;
        const auto sourceId = registry.Add(TInfoUnit("key"));
        TMapIUs columns;
        columns.Add(sourceId, MakeConstant("Uint64", "1", pos));
        auto hub = TReplicate::Create(MakeIntrusive<TOpMap>(
            MakeIntrusive<TOpEmptySource>(pos), pos, std::move(columns)), pos, registry);
        auto removed = hub->AddOutput();
        auto first = hub->AddOutput();
        auto second = hub->AddOutput();
        removed.Reset(); // Logical ordinals are now 1 and 2, physical outputs 0 and 1.
        const auto firstId = *first->GetOutputIUs().begin();
        const auto secondId = *second->GetOutputIUs().begin();
        first->Props.Analysis.LiveOut = TUnorderedIUs{firstId};
        second->Props.Analysis.LiveOut = TUnorderedIUs{secondId};
        // Visit the secondary ordinal first; lowering still follows logical
        // port order, not traversal order or reference counts.
        TOpRoot root(MakeIntrusive<TOpJoin>(second, first, pos, "Cross", TJoinIUs{}), pos, {});
        root.ComputeParents();
        const TPhysicalNames names(registry);
        const auto stream = ctx.NewArgument(pos, "input");
        const auto fanout = NPhysicalConvertionUtils::BuildSwitch(stream, *hub, names, ctx);
        UNIT_ASSERT(fanout->IsCallable("Switch"));
        UNIT_ASSERT_VALUES_EQUAL(fanout->ChildrenSize(), 6);
        for (ui32 index = 0; index < 2; ++index) {
            const auto& fields = fanout->Child(3 + 2 * index)->Tail().Tail().Tail();
            UNIT_ASSERT_VALUES_EQUAL(fields.Head().Head().Content(), names.Get(index ? secondId : firstId));
            UNIT_ASSERT_VALUES_EQUAL(fields.Head().Tail().Tail().Content(), names.Get(sourceId));
        }

        second->NativeSchema = TNativeSchema{{{secondId, {NFrontend::ETypeKind::Uint64}}}, std::nullopt,
            std::make_shared<NFrontend::TExpressionArena>()};
        TOpTableLookup lookup(std::move(second), pos, TRboTable{}, {}, {{secondId, "storage_key"}});
        const auto keys = NLookupJoinBuilder::BuildLookupKeys(lookup, fanout, ctx, names);
        UNIT_ASSERT(keys.InputStage->HeadPtr() == stream);
        UNIT_ASSERT(keys.InputStage->ChildPtr(3) == fanout->ChildPtr(3));
        UNIT_ASSERT(keys.InputStage->ChildPtr(5) != fanout->ChildPtr(5));
        UNIT_ASSERT_VALUES_EQUAL(keys.InputStage->Child(5)->Tail().Tail().Tail().Head().Head().Content(), "storage_key");
    }

    Y_UNIT_TEST(ReplicateOutputIndexFollowsReachabilityAndStageBoundaries) {
        NTests::TIdTestContext f;
        auto read = f.Read({f.Id()});
        auto hub = TReplicate::Create(read, f.Pos, f.Props.InfoUnitRegistry);
        auto discarded = hub->AddOutput();
        auto left = hub->AddOutput(), right = hub->AddOutput();
        discarded.Reset();
        auto map = f.Copies(right, {});
        auto tail = f.Copies(map, {});
        auto root = f.Root(MakeIntrusive<TOpJoin>(tail, left, f.Pos, "Cross", TJoinIUs{}), {});
        UNIT_ASSERT(!GetReplicateOutputIndex(*tail)); // Stages are not assigned yet.
        read->Props.StageId = left->Props.StageId = right->Props.StageId = 0;
        map->Props.StageId = tail->Props.StageId = 0;
        root->GetInput()->Props.StageId = 1;
        UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*left).value(), 0);
        UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*tail).value(), 1);
        UNIT_ASSERT(!GetReplicateOutputIndex(*read));
        UNIT_ASSERT(!GetReplicateOutputIndex(*root->GetInput()));

        map->Props.StageId = 1;
        UNIT_ASSERT(!GetReplicateOutputIndex(*tail)); // Do not cross a stage boundary.
        map->Props.StageId = 0;

        // Reachability changes the physical number without changing the
        // logical port identity or updating any output properties.
        root->GetInput() = tail;
        root->ComputeParents();
        UNIT_ASSERT_VALUES_EQUAL(right->GetIndex(), 2);
        UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*right).value(), 0);
        UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*tail).value(), 0);
    }

    Y_UNIT_TEST(ReplicatePushesPartialAggregationIntoOnlyItsOwnBranch) {
        struct TCase {
            bool Scalar;
            bool Distinct;
            bool EliminateShuffle;
            bool SinglePort = false;
        };
        for (const auto& testCase : TVector<TCase>{{true, false, false}, {false, false, false},
                {false, true, false}, {false, false, true}, {false, false, false, true}}) {
            for (const auto storage : {NYql::EStorageType::RowStorage, NYql::EStorageType::ColumnStorage}) {
                NTests::TIdTestContext f;
                f.Config->SetEnableQueryServiceSpilling(true);
                f.Config->_KqpEnableSpilling = true;
                f.QueryCtx->Type = EKikimrQueryType::Query;
                f.Config->OptShuffleElimination = testCase.EliminateShuffle;
                const auto key = f.Id("key"), value = f.Id("value"), result = f.Id("result");
                auto read = f.Read({key, value});
                read->StorageType = storage;
                f.SetType(*read);
                auto hub = TReplicate::Create(read, f.Pos, f.Props.InfoUnitRegistry);
                auto discarded = hub->AddOutput();
                auto raw = hub->AddOutput(), aggregated = hub->AddOutput();
                discarded.Reset();
                const auto aggregateKey = *aggregated->GetRebindings().Find(key);
                const auto aggregateValue = *aggregated->GetRebindings().Find(value);
                const auto rawValue = *raw->GetRebindings().Find(value);
                f.SetType(*aggregated);
                aggregated->Props.Metadata.emplace();
                if (testCase.EliminateShuffle) {
                    aggregated->Props.Metadata->ShuffledByColumns = TOrderedIUs<>{aggregateKey};
                }
                TAggregationIUs traits;
                traits.Add(result, TOpAggregationTraits{
                    testCase.Distinct ? aggregateKey : aggregateValue, testCase.Distinct ? "distinct" : "sum"});
                auto aggregate = MakeIntrusive<TOpAggregate>(aggregated, std::move(traits),
                    testCase.Scalar ? TOrderedIUs<>{} : TOrderedIUs<>{aggregateKey},
                    EOpPhase::Undefined, testCase.Distinct, f.Pos);
                // Traverse physical output one before output zero, with both
                // logical ordinals shifted by the discarded primary port.
                auto root = testCase.SinglePort ? f.Root(aggregate, {{result, "result"}})
                    : f.Root(MakeIntrusive<TOpJoin>(aggregate, raw, f.Pos, "Cross", TJoinIUs{}),
                        {{result, "result"}, {rawValue, "raw"}});
                TAssignStagesStage().RunStage(*root, f.RboCtx);
                const auto producerStage = *read->Props.StageId;
                const auto finalStage = *aggregate->Props.StageId;
                const ui32 outputIndex = testCase.SinglePort ? 0 : 1;
                auto& aggregateSlot = testCase.SinglePort ? root->GetInput()
                    : CastOperator<TOpJoin>(*root->GetInput()).GetLeftInput();
                TPropagateAggregateThroughStageRule rule;
                UNIT_ASSERT(rule.MatchAndApply(aggregateSlot, f.RboCtx, root->PlanProps));
                auto& final = CastOperator<TOpAggregate>(*aggregateSlot);
                UNIT_ASSERT(rule.MatchAndApply(final.GetInput(), f.RboCtx, root->PlanProps));
                auto& partial = CastOperator<TOpAggregate>(*final.GetInput());
                UNIT_ASSERT(partial.GetInput() == aggregated);
                UNIT_ASSERT(partial.GetAggregationPhase() == EOpPhase::Intermediate);
                UNIT_ASSERT_VALUES_EQUAL(*partial.Props.StageId, producerStage);
                UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(partial).value(), outputIndex);
                UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*aggregated).value(), outputIndex);
                UNIT_ASSERT_VALUES_EQUAL(*final.Props.StageId, finalStage);
                UNIT_ASSERT(!GetReplicateOutputIndex(final));
                UNIT_ASSERT(!GetReplicateOutputIndex(*read));
                UNIT_ASSERT(hub->GetInput() == read);
                UNIT_ASSERT(aggregated->GetOutputIUs() == (TUnorderedIUs{aggregateKey, aggregateValue}));
                UNIT_ASSERT(raw->GetInput() == read);
                const auto& connection = root->PlanProps.StageGraph.GetConnections(producerStage, finalStage).front();
                UNIT_ASSERT_VALUES_EQUAL(connection->GetOutputIndex(), outputIndex);
                if (testCase.Scalar) {
                    UNIT_ASSERT(IsConnection<TUnionAllConnection>(connection));
                } else if (testCase.EliminateShuffle) {
                    UNIT_ASSERT(IsConnection<TMapConnection>(connection));
                } else {
                    UNIT_ASSERT(IsConnection<TShuffleConnection>(connection));
                    UNIT_ASSERT(connection->GetUsedIUs() == (testCase.Distinct
                        ? partial.GetAggregationTraits().Keys() : TUnorderedIUs{aggregateKey}));
                }
                // The partial is an ordinary node, visible to both liveness
                // modes and the normal SSA checks.
                ComputePlanLiveness(*root, ELivenessMode::Global);
                NTests::AssertIdInvariants(*root, root->PlanProps);
                ComputePlanLiveness(*root);
                UNIT_ASSERT(GetLiveOut(aggregated.Get()) == GetLiveIn(&partial, 0));
                f.SetType(*partial.GetInput());
                f.SetType(partial);
                const TPhysicalNames names(root->PlanProps.InfoUnitRegistry);
                auto stage = TPhysicalSourceBuilder(*read, f.ExprCtx, f.Pos, names,
                    root->PlanProps.InfoUnitRegistry, "stage").BuildPhysicalOp();
                stage = NPhysicalConvertionUtils::BuildSwitch(stage, *hub, names, f.ExprCtx);
                // A sibling consumer may have adapted its branch before this
                // aggregate is visited in the normal postorder traversal.
                if (!testCase.SinglePort) {
                    f.SetType(*raw);
                    TOpTableLookup rawLookup(raw, f.Pos, TRboTable{}, {}, {{rawValue, "raw_storage_key"}});
                    stage = NLookupJoinBuilder::BuildLookupKeys(rawLookup, stage, f.ExprCtx, names).InputStage;
                }
                const auto before = storage == NYql::EStorageType::RowStorage
                    ? TDqPhyStage(stage).Program().Body().Ptr() : stage;
                stage = NPhysicalConvertionUtils::TransformStageOutput(stage, [&](TExprNode::TPtr body) {
                    return TPhysicalAggregationBuilder(partial, f.ExprCtx, f.Pos, names)
                        .BuildPhysicalOp(body, std::nullopt);
                }, GetReplicateOutputIndex(partial), f.ExprCtx);
                const auto body = storage == NYql::EStorageType::RowStorage
                    ? TDqPhyStage(stage).Program().Body().Ptr() : stage;
                UNIT_ASSERT_VALUES_EQUAL(body->IsCallable("Switch"), !testCase.SinglePort);
                if (!testCase.SinglePort) {
                    UNIT_ASSERT(body->HeadPtr() == before->HeadPtr());
                    UNIT_ASSERT(body->ChildPtr(3) == before->ChildPtr(3));
                    UNIT_ASSERT(!FindNode(body->ChildPtr(3), [](const auto& node) {
                        return node->IsCallable("DqPhyHashCombine");
                    }));
                }
                UNIT_ASSERT(FindNode(testCase.SinglePort ? body : body->ChildPtr(5), [](const auto& node) {
                    return node->IsCallable("DqPhyHashCombine");
                }));
                if (storage == NYql::EStorageType::RowStorage) {
                    UNIT_ASSERT(TDqPhyStage::Match(stage.Get()));
                }

                // Lookup key adaptation follows the aggregate's output index.
                const auto lookupKey = *partial.GetAggregationTraits().Keys().begin();
                TOpTableLookup lookup(final.GetInput(), f.Pos, TRboTable{}, {}, {{lookupKey, "storage_key"}});
                const auto keys = NLookupJoinBuilder::BuildLookupKeys(lookup, stage, f.ExprCtx, names);
                const auto lookupBody = storage == NYql::EStorageType::RowStorage
                    ? TDqPhyStage(keys.InputStage).Program().Body().Ptr() : keys.InputStage;
                if (!testCase.SinglePort) {
                    UNIT_ASSERT(lookupBody->HeadPtr() == body->HeadPtr());
                    UNIT_ASSERT(lookupBody->ChildPtr(3) == body->ChildPtr(3));
                }
                const auto lookupBranch = testCase.SinglePort ? lookupBody : lookupBody->Child(5)->TailPtr();
                UNIT_ASSERT(lookupBranch->IsCallable("Map"));
                UNIT_ASSERT(lookupBranch->HeadPtr() == (testCase.SinglePort ? body : body->Child(5)->TailPtr()));
                UNIT_ASSERT_VALUES_EQUAL(lookupBranch->Tail().Tail().Head().Head().Content(), "storage_key");

                for (const auto& item : *root) {
                    item.Current->Props.Statistics.emplace();
                    item.Current->Props.Cost = 0;
                }
                for (const auto& [stages, connections] : root->PlanProps.StageGraph.Connections) {
                    for (const auto& connection : connections) {
                        if (IsConnection<TShuffleConnection>(connection)) {
                            static_cast<TShuffleConnection&>(*connection).HashFuncType = NYql::NDq::EHashShuffleFuncType::HashV2;
                        }
                    }
                }
                ui64 nodeId = 0;
                ui32 operatorIndex = 0;
                THashMap<IOperator*, ui32> operatorIds;
                root->GetExecutionJson(nodeId, operatorIndex, operatorIds);
                UNIT_ASSERT(operatorIds.contains(&partial));
                const auto explain = root->GetExplainJson(nodeId, operatorIds);
                const auto hasPartial = [&](auto&& self, const NJson::TJsonValue& node) -> bool {
                    if (node["Operators"].IsArray()) {
                        for (const auto& op : node["Operators"].GetArraySafe()) {
                            if (op["OperatorId"].GetIntegerSafe() == operatorIds.at(&partial)
                                && op["Phase"].GetStringSafe() == "Intermediate") {
                                return true;
                            }
                        }
                    }
                    if (node["Plans"].IsArray()) {
                        for (const auto& child : node["Plans"].GetArraySafe()) {
                            if (self(self, child)) {
                                return true;
                            }
                        }
                    }
                    return false;
                };
                UNIT_ASSERT(hasPartial(hasPartial, explain));
            }
        }
    }

    Y_UNIT_TEST(StageAssignmentAndLivenessDistinguishReplicateEdgesToOneJoin) {
        NTests::TIdTestContext f;
        f.Config->SetEnableQueryServiceSpilling(true);
        f.Config->_KqpEnableSpilling = true;
        f.QueryCtx->Type = EKikimrQueryType::Query;
        const auto a = f.Id("left"), b = f.Id("right");
        auto hub = TReplicate::Create(f.Read({a, b}), f.Pos, f.Props.InfoUnitRegistry);
        auto discarded = hub->AddOutput();
        auto left = hub->AddOutput(), right = hub->AddOutput();
        discarded.Reset();
        auto* leftPort = left.get();
        auto* rightPort = right.get();
        const auto leftKey = *left->GetRebindings().Find(a);
        const auto rightKey = *right->GetRebindings().Find(b);
        auto root = f.Root(MakeIntrusive<TOpJoin>(std::move(left), std::move(right), f.Pos, "Inner",
            TPairedIUs{{leftKey, rightKey}}), {});
        root->GetInput()->Props.JoinAlgo = EJoinAlgoType::GraceJoin;
        root->GetInput()->Props.UseBlockHashJoin = false;
        TAssignStagesStage().RunStage(*root, f.RboCtx);
        UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*leftPort).value(), 0);
        UNIT_ASSERT_VALUES_EQUAL(GetReplicateOutputIndex(*rightPort).value(), 1);
        UNIT_ASSERT_VALUES_EQUAL(root->PlanProps.StageGraph.StageIds.size(), 2);
        ComputePlanLiveness(*root);
        UNIT_ASSERT(GetLiveOut(leftPort) == TUnorderedIUs{leftKey});
        UNIT_ASSERT(GetLiveOut(rightPort) == TUnorderedIUs{rightKey});
        UNIT_ASSERT(GetLiveOut(hub->GetInput().Get()) == (TUnorderedIUs{a, b}));

        for (const auto& item : *root) {
            item.Current->Props.Statistics.emplace();
            item.Current->Props.Cost = 0;
        }
        const auto& connections = root->PlanProps.StageGraph.GetConnections(
            *leftPort->Props.StageId, *root->GetInput()->Props.StageId);
        UNIT_ASSERT_VALUES_EQUAL(connections.size(), 2);
        for (const auto& connection : connections) {
            static_cast<TShuffleConnection&>(*connection).HashFuncType = NYql::NDq::EHashShuffleFuncType::HashV2;
        }
        ui64 nodeId = 0;
        ui32 operatorIndex = 0;
        THashMap<IOperator*, ui32> operatorIds;
        const auto json = root->GetExecutionJson(nodeId, operatorIndex, operatorIds);
        const auto& inputs = json["Plans"].GetArraySafe().front()["Plans"].GetArraySafe();
        UNIT_ASSERT_VALUES_EQUAL(inputs.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(inputs[0]["KeyColumns"].GetArraySafe().front().GetStringSafe(), root->PlanProps.InfoUnitRegistry.GetDisplayName(leftKey));
        UNIT_ASSERT_VALUES_EQUAL(inputs[1]["KeyColumns"].GetArraySafe().front().GetStringSafe(), root->PlanProps.InfoUnitRegistry.GetDisplayName(rightKey));
        UNIT_ASSERT(inputs[0]["PlanNodeId"].GetIntegerSafe() != inputs[1]["PlanNodeId"].GetIntegerSafe());
    }
}

} // namespace NKikimr::NKqp
