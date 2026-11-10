#include "kqp_rbo_physical_query_builder.h"
#include "kqp_rbo_compatibility.h"
#include "kqp_rbo_physical_layout.h"
#include "kqp_rbo_physical_table_effect_builder.h"
#include <ydb/core/kqp/opt/rbo/frontend_lowering.h>

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/opt/rbo/kqp_operator.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_context.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_convertion_utils.h>
#include <ydb/core/kqp/opt/rbo/traces/kqp_rbo_yql_ast_trace.h>
#include <ydb/core/kqp/provider/yql_kikimr_settings.h>
#include <ydb/library/yql/dq/opt/dq_opt_build.h>
#include <ydb/library/yql/dq/type_ann/dq_type_ann.h>

#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

#include <cctype>
#include <optional>
#include <sstream>

namespace NKikimr::NKqp {

using namespace NYql;
using namespace NYql::NNodes;
using namespace NKikimr;

namespace {

std::string TraceRootId(const std::string& title) {
    std::string id = "physical-ast";
    for (const char ch : title) {
        id += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '-');
    }
    return id;
}

std::string ToStdString(TStringBuf value) {
    return std::string(value.data(), value.size());
}

std::string FormatExprForTrace(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (!node) {
        return {};
    }

    return ToStdString(KqpExprToPrettyString(*node, ctx));
}

std::string FormatPhysicalStagesForTrace(const TVector<TExprNode::TPtr>& stages, TExprContext& ctx) {
    std::ostringstream out;
    for (size_t i = 0; i < stages.size(); ++i) {
        if (i) {
            out << "\n";
        }
        out << "----- Stage " << i << " -----\n"
            << FormatExprForTrace(stages[i], ctx)
            << "\n";
    }

    return out.str();
}

void AddAstInfoTabs(
    optimizer_trace::Trace::Tile& tile,
    const std::optional<optimizer_trace::Widget>& linkGraph,
    const std::string& text)
{
    if (linkGraph) {
        tile.info().tab("dag-links", "DAG links")
            .widget(*linkGraph);
    }
    if (!text.empty()) {
        tile.info().tab("yql-ast-text", "YQL AST text")
            .widget(optimizer_trace::Widget::unwrappedText("Regular YQL AST", text, true));
    }
}

void SubmitPhysicalAstTrace(TRBOContext& rboCtx, const std::string& title, NYqlAstTrace::TBuildResult astTrace, std::string text) {
    if (!rboCtx.NeedToLog()) {
        return;
    }

    auto& tile = rboCtx.TraceLog.currentStage().tree(title, astTrace.Root);
    AddAstInfoTabs(tile, astTrace.LinkGraph, text);
    rboCtx.TraceLog.Submit(tile);
}

void SubmitPhysicalStagesTrace(TRBOContext& rboCtx, const std::string& title, const TVector<TExprNode::TPtr>& stages) {
    if (!rboCtx.NeedToLog()) {
        return;
    }

    SubmitPhysicalAstTrace(
        rboCtx,
        title,
        NYqlAstTrace::BuildStageListTreeWithInfo(stages, TraceRootId(title)),
        FormatPhysicalStagesForTrace(stages, rboCtx.ExprCtx));
}

void SubmitPhysicalExprTrace(TRBOContext& rboCtx, const std::string& title, const TExprNode::TPtr& node) {
    if (!rboCtx.NeedToLog() || !node) {
        return;
    }

    SubmitPhysicalAstTrace(
        rboCtx,
        title,
        NYqlAstTrace::BuildExprTreeWithInfo(node, TraceRootId(title)),
        FormatExprForTrace(node, rboCtx.ExprCtx));
}

} // anonymous namespace

TPhysicalQueryBuilder::TPhysicalQueryBuilder(const TVector<TIntrusivePtr<TOpRoot>>& roots,
    TVector<TStageGraph>&& graphs, TVector<THashMap<ui32, TExprNode::TPtr>>&& stages, 
    TVector<THashMap<ui32, TVector<TExprNode::TPtr>>>&& stageArgs,
    TVector<THashMap<ui32, TPositionHandle>>&& stagePos, TVector<TPhysicalNames>&& names, TRBOContext& rboCtx)
    : Roots(roots)
    , Graphs(std::move(graphs))
    , Stages(std::move(stages))
    , StageArgs(std::move(stageArgs))
    , StagePos(std::move(stagePos))
    , Names(std::move(names))
    , RBOCtx(rboCtx)
{
    Materialize.resize(roots.size());
}

TExprNode::TPtr TPhysicalQueryBuilder::BuildPhysicalQuery() {
    TVector<TVector<TExprNode::TPtr>> allPhyStages;

    for (size_t i=0; i<Roots.size(); i++) {
        auto phyStages = BuildPhysicalStageGraph(i);
        SubmitPhysicalStagesTrace(RBOCtx, "After physical stage graph build", phyStages);
        phyStages = PreparePhysicalStages(std::move(phyStages));
        SubmitPhysicalStagesTrace(RBOCtx, "After physical stage preparation", phyStages);
        phyStages = LowerPhysicalStageCompatibility(std::move(phyStages));
        SubmitPhysicalStagesTrace(RBOCtx, "After execution lowering", phyStages);
        allPhyStages.push_back(std::move(phyStages));
    }
    auto physicalQuery = BuildPhysicalQuery(std::move(allPhyStages));
    SubmitPhysicalExprTrace(RBOCtx, "Final physical query", physicalQuery);
    return physicalQuery;
}

TExprNode::TPtr TPhysicalQueryBuilder::LowerExecutionCompatibility(TExprNode::TPtr root) {
    if (!root->GetTypeAnn()) TypeAnnotate(root);
    if (!NeedsRboCompatibilityLowering(root)) return root;
    TOptimizeExprSettings settings(&RBOCtx.TypeCtx);
    settings.CustomInstantTypeTransformer = RBOCtx.TypeCtx.CustomInstantTypeTransformer.Get();
    constexpr size_t MaxPasses = 64;
    for (size_t pass = 0; pass < MaxPasses && NeedsRboCompatibilityLowering(root); ++pass) {
        TExprNode::TPtr output;
        const auto status = OptimizeExpr(
            root,
            output,
            [&](const TExprNode::TPtr& node, TExprContext& ctx) {
                return RewriteRboCompatibilityNode(node, ctx, RBOCtx.TypeCtx);
            },
            RBOCtx.ExprCtx,
            settings);
        YQL_ENSURE(status != IGraphTransformer::TStatus::Error,
            "Failed to lower execution-incompatible callables in new RBO physical stages");
        if (status == IGraphTransformer::TStatus::Ok) {
            break;
        }

        YQL_ENSURE(status == IGraphTransformer::TStatus::Repeat);
        YQL_ENSURE(output != root, "RBO compatibility lowering made no progress");
        root = std::move(output);
        TypeAnnotate(root);
    }

    EnsureRboCompatibilityLowered(root);
    return root;
}

TVector<TExprNode::TPtr> TPhysicalQueryBuilder::LowerPhysicalStageCompatibility(TVector<TExprNode::TPtr>&& physicalStages) {
    Y_ENSURE(!physicalStages.empty());
    auto root = LowerExecutionCompatibility(physicalStages.back());
    if (root == physicalStages.back()) return std::move(physicalStages);
    TVector<TExprNode::TPtr> stagesTopSorted;
    TopologicalSort(TDqPhyStage(root), stagesTopSorted);
    return stagesTopSorted;
}

TVector<TExprNode::TPtr> TPhysicalQueryBuilder::BuildPhysicalStageGraph(int rootIdx) {
    TVector<TExprNode::TPtr> phyStages;
    auto& graph = Graphs[rootIdx];
    graph.TopologicalSort();
    const auto& stageIds = graph.StageIds;
    const auto& stageInputIds = graph.StageInputs;
    auto& ctx = RBOCtx.ExprCtx;

    THashMap<ui32, TExprNode::TPtr> finalizedStages;
    for (const auto id : stageIds) {
        YQL_CLOG(TRACE, CoreDq) << "Finalizing stage " << id;

        TVector<TExprNode::TPtr> inputConnections;
        THashSet<ui32> processedInputsIds;
        for (const auto inputStageId : stageInputIds.at(id)) {
            if (processedInputsIds.contains(inputStageId)) {
                continue;
            }
            processedInputsIds.insert(inputStageId);

            const auto inputStage = finalizedStages.at(inputStageId);
            const auto connections = graph.GetConnections(inputStageId, id);
            for (const auto& connection : connections) {
                YQL_CLOG(TRACE, CoreDq) << "Building connection: " << inputStageId << "->" << id << ", " << connection->Type;
                auto dqConnection = connection->BuildConnection(inputStage, StagePos[rootIdx].at(inputStageId), ctx, Names[rootIdx]);
                YQL_CLOG(TRACE, CoreDq) << "Built connection: " << inputStageId << "->" << id << ", " << connection->Type;
                inputConnections.push_back(dqConnection);
            }
        }

        TExprNode::TPtr stage;
        if (graph.IsSourceStageRowType(id)) {
            stage = Stages[rootIdx].at(id);
            // Computed ranges are materialized in a separate transaction. A literal range is taken by the source as is.
            auto rowSettingsPtr = FindNode(stage, [](const TExprNode::TPtr& node) { return !!TMaybeNode<TKqpReadRangesSourceSettings>(node); });
            if (rowSettingsPtr) {
                auto rowSettings = TExprBase(rowSettingsPtr).Cast<TKqpReadRangesSourceSettings>();
                if (!rowSettings.RangesExpr().Maybe<TCoVoid>() && !rowSettings.RangesExpr().Maybe<TKqlKeyRange>()) {
                    const auto materializeResult = BuildMaterialize(rootIdx, rowSettings.RangesExpr().Ptr());
                    // clang-fomrat off
                    const auto newRowSettings = Build<TKqpReadRangesSourceSettings>(ctx, rowSettingsPtr->Pos())
                        .Table(rowSettings.Table())
                        .Columns(rowSettings.Columns())
                        .Settings(rowSettings.Settings())
                        .RangesExpr(materializeResult)
                        .ExplainPrompt(rowSettings.ExplainPrompt())
                    .Done().Ptr();
                    // clang-format on
                    stage = ctx.ReplaceNode(std::move(stage), rowSettings.Ref(), newRowSettings);
                }
            }
        } else {
            TVector<TExprNode::TPtr> stageInputConnections;
            TVector<TExprNode::TPtr> stageInputArgs;
            if (!graph.IsSourceStageColumnType(id)) {
                stageInputConnections = inputConnections;
                stageInputArgs = StageArgs[rootIdx].at(id);
            }

            auto stageGUID = graph.StageGUIDs.at(id);
            if (graph.IsSinkStage(id)) {
                const auto& effect = CastOperator<TOpTableEffect>(*Roots[rootIdx]->GetInput());
                const auto pos = StagePos[rootIdx].at(id);
                auto body = Stages[rootIdx].at(id);
                if (effect.Options.DefaultColumns && !effect.Options.DefaultColumns->empty()) {
                    // Defaults extend the storage row after ordinary input
                    // evaluation. This is an execution connection, not a
                    // logical rewrite or an expression in the optimizer.
                    auto input = BuildDqPhyStage(stageInputConnections, stageInputArgs, body,
                        NYql::NDq::TDqStageSettings::New().BuildNode(ctx, pos), ctx, pos);
                    TypeAnnotate(input);
                    phyStages.push_back(input);
                    stageInputConnections = {LowerTableDefaultConnection(input, effect, ctx)};
                    auto argument = ctx.NewArgument(pos, "default_rows");
                    stageInputArgs = {argument};
                    body = Build<TCoToStream>(ctx, pos).Input(argument).Done().Ptr();
                }
                body = LowerTableGeneratedColumns(std::move(body), effect, ctx);
                stage = BuildDqPhySinkStage(stageInputConnections, stageInputArgs, body,
                    NYql::NDq::TDqStageSettings::New(stageGUID).BuildNode(ctx, pos),
                    LowerTableSinkSettings(graph.GetSinkSettings(id), ctx, pos, Roots[rootIdx]->PlanProps.EffectPriority),
                    effect, ctx, pos);
            }
            else {
                stage = BuildDqPhyStage(stageInputConnections, stageInputArgs, Stages[rootIdx].at(id), NYql::NDq::TDqStageSettings().New(stageGUID).BuildNode(ctx, StagePos[rootIdx].at(id)),
                                    ctx, StagePos[rootIdx].at(id));
            }
            phyStages.emplace_back(stage);
            YQL_CLOG(TRACE, CoreDq) << "Added stage " << stage->UniqueId();
        }

        if (graph.IsSourceStageColumnType(id)) {
            auto readPtr = FindNode(stage, [](const TExprNode::TPtr& node) { return !!TMaybeNode<TKqpBlockReadOlapTableRanges>(node); });
            if (readPtr) {
                auto read = TExprBase(readPtr).Cast<TKqpBlockReadOlapTableRanges>();
                if (!read.Ranges().Maybe<TCoVoid>()) {
                    const auto materializeResult = BuildMaterialize(rootIdx, read.Ranges().Ptr());
                    // clang-fomrat off
                    const auto newRead = Build<TKqpBlockReadOlapTableRanges>(ctx, readPtr->Pos())
                        .Table(read.Table())
                        .Ranges(materializeResult)
                        .Columns(read.Columns())
                        .Settings(read.Settings())
                        .ExplainPrompt(read.ExplainPrompt())
                        .Process(read.Process())
                    .Done().Ptr();
                    // clang-format on
                    stage = ctx.ReplaceNode(std::move(stage), read.Ref(), newRead);
                    phyStages.back() = stage;
                }
            }
        }

        finalizedStages[id] = stage;
        YQL_CLOG(TRACE, CoreDq) << "Finalized stage " << id;
    }

    // The root stage is last in the topologically sorted StageIds. It is usually also phyStages.back(),
    // but a lone row-source stage is built into Stages without being appended to phyStages (see above),
    // so deriving the final stage from finalizedStages is correct even when phyStages is empty.
    Y_ENSURE(!stageIds.empty());
    auto maybeFinalStage = finalizedStages.at(stageIds.back());
    if (Roots[rootIdx]->PlanProps.WithReturning) {
        // ReturningSink exposes storage column names. Introduce the returned
        // IU definitions at this boundary before ordinary result projection.
        const auto& effect = CastOperator<TOpTableEffect>(*Roots[rootIdx]->GetInput());
        auto connection = Build<TDqCnUnionAll>(ctx, effect.Pos)
            .Output().Stage(maybeFinalStage).Index().Build("0").Build().Done();
        auto arg = ctx.NewArgument(effect.Pos, "returned_rows");
        TVector<std::pair<TString, TString>> columns;
        for (const auto& [id, name] : effect.GetReturningColumns().Items()) {
            columns.emplace_back(name, Names[rootIdx].Get(id));
        }
        auto resultRows = NPhysicalConvertionUtils::BuildRenameMap(arg, columns, ctx, /*ordered=*/true);
        if (const auto& limit = Roots[rootIdx]->PlanProps.ReturningRowsLimit) {
            resultRows = Build<TCoTake>(ctx, effect.Pos).Input(resultRows).Count<TCoUint64>().Literal().Build(ToString(*limit)).Build().Done().Ptr();
        }
        maybeFinalStage = BuildDqPhyStage({connection.Ptr()}, {arg}, resultRows,
            NYql::NDq::TDqStageSettings().New().BuildNode(ctx, effect.Pos), ctx, effect.Pos);
        phyStages.push_back(maybeFinalStage);
    } else if (Roots[rootIdx]->PlanProps.WithEffects) {
        // A sink consumes its stream and exposes no result output. In
        // particular, do not add a union/result stage on its output zero.
        Y_ENSURE(!phyStages.empty() && phyStages.back() == maybeFinalStage);
        return phyStages;
    }
    const auto finalStage = GetFinalStage(maybeFinalStage);
    const bool needFinalNarrowing = NeedFinalNarrowing(rootIdx);
    const auto finalResultStage = needFinalNarrowing ? BuildFinalNarrowStage(rootIdx, finalStage) : finalStage;
    if (finalStage.Get() != maybeFinalStage.Get()) {
        phyStages.push_back(finalResultStage);
    } else if (needFinalNarrowing) {
        Y_ENSURE(!phyStages.empty());
        Y_ENSURE(phyStages.back().Get() == finalStage.Get());
        phyStages.back() = finalResultStage;
    }

    return phyStages;
}

TExprNode::TPtr TPhysicalQueryBuilder::BuildMaterialize(int rootIdx, TExprNode::TPtr node) {
    auto& ctx = RBOCtx.ExprCtx;

    // Materialization packages a native expression after the same owned
    // execution lowering used for physical stages. No YQL peephole is invoked.
    node = LowerExecutionCompatibility(std::move(node));

    // clang-format off
    auto rangesProgram = Build<TCoToStream>(ctx, node->Pos())
        .Input<TCoJust>()
            .Input<TExprList>()
                .Add({node})
            .Build()
        .Build()
    .Done().Ptr();
    // clang-format on

    auto stageSettings = NYql::NDq::TDqStageSettings().New().SetPartitionMode(NYql::NDq::TDqStageSettings::EPartitionMode::Single).BuildNode(ctx, node->Pos());
    auto phyStage = BuildDqPhyStage({}, {}, rangesProgram, std::move(stageSettings), ctx, node->Pos());

    // clang-format off
    auto result = Build<TDqCnValue>(ctx, node->Pos())
        .Output()
            .Stage(phyStage)
            .Index().Build("0")
        .Build()
    .Done().Ptr();
    // clang-format on

    TypeAnnotate(result);
    Y_ENSURE(result->GetTypeAnn());

    // clang-format off
    auto param = Build<TCoParameter>(ctx, node->Pos())
        .Name<TCoAtom>()
            .Value(ParamBindingName + ToString(UniqueParamsId++))
        .Build()
        .Type(ExpandType(node->Pos(), *result->GetTypeAnn(), ctx))
    .Done().Ptr();
    // clang-format on

    Materialize[rootIdx].push_back({param, result});
    return param;
}

bool TPhysicalQueryBuilder::IsSingleTaskConnection(const TExprBase& input) const {
    return input.Maybe<TDqCnUnionAll>() || input.Maybe<TDqCnMerge>();
}

bool TPhysicalQueryBuilder::NeedFinalNarrowing(int rootIdx) {
    auto& root = *Roots[rootIdx];
    const auto& outputIUs = root.GetInput()->GetOutputIUs();
    if (outputIUs.Size() != root.GetColumns().Items().size()) {
        return true;
    }

    for (const auto& [id, label] : root.GetColumns().Items()) {
        if (!outputIUs.Contains(id) || Names[rootIdx].Get(id) != label) {
            return true;
        }
    }

    return false;
}

TExprNode::TPtr TPhysicalQueryBuilder::GetFinalStage(const TExprNode::TPtr& stage) const {
    auto& ctx = RBOCtx.ExprCtx;
    TExprNode::TPtr finalStage;
    bool needFinalUnionStage = false;

    const auto inputs = TDqPhyStage(stage).Inputs();
    // If no inputs - need a final stage.
    if (inputs.Empty()) {
        needFinalUnionStage = true;
    } else {
        // Final stage, which is input for DqCnResult, should have only one 1 task.
        for (const auto& input : TDqPhyStage(stage).Inputs()) {
            if (!IsSingleTaskConnection(input)) {
                needFinalUnionStage = true;
                break;
            }
        }
    }

    if (needFinalUnionStage) {
        // clang-format off
        auto input = Build<TDqCnUnionAll>(ctx, stage->Pos())
            .Output()
                .Stage(stage)
                .Index().Build("0")
                .Build()
            .Done().Ptr();

        finalStage = Build<TDqPhyStage>(ctx, stage->Pos())
            .Inputs()
                .Add({input})
            .Build()
            .Program<TCoLambda>()
                .Args({"arg"})
                .Body("arg")
            .Build()
            .Settings(NYql::NDq::TDqStageSettings().New().BuildNode(ctx, stage->Pos()))
        .Done().Ptr();
    // clang-format on
    } else {
        finalStage = stage;
    }
    return finalStage;
}

TExprNode::TPtr TPhysicalQueryBuilder::BuildFinalNarrowStage(int rootIdx, const TExprNode::TPtr& stage) const {
    auto& ctx = RBOCtx.ExprCtx;
    const auto dqStage = TDqPhyStage(stage);

    TVector<std::pair<TString, TString>> finalColumns;
    for (const auto& [id, label] : Roots[rootIdx]->GetColumns().Items()) {
        finalColumns.emplace_back(Names[rootIdx].Get(id), label);
    }

    const auto narrowBody =
        NPhysicalConvertionUtils::BuildRenameMap(dqStage.Program().Body().Ptr(), finalColumns, ctx, /*ordered=*/true);

    // clang-format off
    return Build<TDqPhyStage>(ctx, stage->Pos())
        .InitFrom(dqStage)
        .Program()
            .Args(dqStage.Program().Args())
            .Body(narrowBody)
        .Build()
    .Done().Ptr();
    // clang-format on
}

// Collects param bindings walking from root stage.
TVector<TKqpParamBinding> TPhysicalQueryBuilder::CollectParamBindings(int rootIdx, const TExprNode::TPtr& rootStage) {
    auto& ctx = RBOCtx.ExprCtx;
    auto pos = Roots[rootIdx]->Pos;

    TVector<TKqpParamBinding> paramBindings;
    THashSet<TString> paramsCollected;
    const auto params = FindNodes(rootStage, [](const TExprNode::TPtr& node) { return !!TMaybeNode<TCoParameter>(node); });
    for (const auto& param : params) {
        const auto paramName = TExprBase(param).Cast<TCoParameter>().Name().StringValue();
        if (!paramsCollected.contains(paramName) && paramName.find(ParamBindingName) == TString::npos) {
            const TStringBuf randomKind = paramName == QueryRandomUuidParameter ? "RandomUuid" :
                paramName == QueryRandomNumberParameter ? "RandomNumber" : "";
            if (!randomKind.empty()) {
                auto internal = Build<TKqpTxInternalBinding>(ctx, pos)
                    .Type(TCoParameter(param).Type()).Kind().Build(randomKind).Done();
                paramBindings.push_back(Build<TKqpParamBinding>(ctx, pos)
                    .Name().Build(paramName).Binding(internal).Done());
            } else {
                paramBindings.push_back(Build<TKqpParamBinding>(ctx, pos)
                    .Name().Build(paramName).Done());
            }
            paramsCollected.insert(paramName);
        }
    }

    return paramBindings;
}

TExprNode::TPtr TPhysicalQueryBuilder::BuildPhysicalQuery(TVector<TVector<TExprNode::TPtr>>&& physicalStages) {
    Y_ENSURE(physicalStages.size() == Roots.size() && !Roots.empty());
    auto& ctx = RBOCtx.ExprCtx;
    const auto pos = Roots.front()->Pos;
    TVector<TExprNode::TPtr> transactions;
    TVector<TExprNode::TPtr> resultBindings;
    TVector<std::pair<ui32, TExprNode::TPtr>> orderedResults;
    THashMap<TString, TExprNode::TPtr> materializedBindings;
    const auto bindParameter = [&](const TKqpParamBinding& parameter) {
        const auto* binding = materializedBindings.FindPtr(parameter.Name().StringValue());
        return binding ? Build<TKqpParamBinding>(ctx, pos)
            .Name(parameter.Name()).Binding(*binding).Done() : parameter;
    };

    // A query block's reads share a snapshot. Its effects also share a
    // snapshot, with sink priorities preserving their write order. Only cross
    // a transaction boundary when the frontend starts another block/phase.
    for (size_t begin = 0; begin < Roots.size();) {
        size_t end = begin + 1;
        while (end < Roots.size() && Roots[end]->PlanProps.PhysicalTxGroup == Roots[begin]->PlanProps.PhysicalTxGroup) {
            ++end;
        }
        TVector<TKqpParamBinding> paramBindings;
        THashSet<TString> paramNames;
        TVector<TExprNode::TPtr> materializeStages;
        TVector<TExprNode::TPtr> materializeResults;
        TVector<TKqpParamBinding> materializeParams;
        THashSet<TString> materializeParamNames;
        for (size_t i = begin; i < end; ++i) {
            for (const auto& parameter : CollectParamBindings(i, physicalStages[i].back())) {
                if (paramNames.insert(parameter.Name().StringValue()).second) {
                    paramBindings.push_back(bindParameter(parameter));
                }
            }
            for (const auto& [paramNode, resultNode] : Materialize[i]) {
                const auto param = TCoParameter(paramNode);
                const auto result = TDqCnValue(resultNode);
                auto binding = Build<TKqpTxResultBinding>(ctx, pos)
                    .Type(ExpandType(pos, *resultNode->GetTypeAnn(), ctx))
                    .TxIndex().Build(ToString(transactions.size()))
                    .ResultIndex().Build(ToString(materializeResults.size()))
                    .Done();
                paramBindings.push_back(Build<TKqpParamBinding>(ctx, pos)
                    .Name(param.Name()).Binding(binding.Ptr()).Done());
                for (const auto& parameter : CollectParamBindings(i, result.Output().Stage().Ptr())) {
                    if (materializeParamNames.insert(parameter.Name().StringValue()).second) {
                        materializeParams.push_back(bindParameter(parameter));
                    }
                }
                materializeStages.push_back(result.Output().Stage().Ptr());
                materializeResults.push_back(resultNode);
            }
        }
        if (!materializeResults.empty()) {
            TKqpPhyTxSettings settings;
            settings.Type = EPhysicalTxType::Compute;
            transactions.push_back(Build<TKqpPhysicalTx>(ctx, pos)
                .Stages().Add(materializeStages).Build()
                .Results().Add(materializeResults).Build()
                .ParamBindings().Add(materializeParams).Build()
                .Settings(settings.BuildNode(ctx, pos)).Done().Ptr());
        }

        TVector<TExprNode::TPtr> stages;
        TVector<TExprNode::TPtr> results;
        const auto settings = GetPhysicalTxSettings(begin);
        for (size_t i = begin; i < end; ++i) {
            Y_ENSURE(Roots[i]->PlanProps.WithEffects == settings.WithEffects);
            stages.insert(stages.end(), physicalStages[i].begin(), physicalStages[i].end());
            if (settings.WithEffects && !Roots[i]->PlanProps.WithReturning) {
                continue;
            }
            TVector<TCoAtom> columns;
            // Export the resolved projection order for SELECT and RETURNING.
            // Original query hints can be empty even for an ordered projection;
            // exporting them would fall back to alphabetical struct-field order.
            for (const auto& [id, label] : Roots[i]->GetColumns().Items()) {
                columns.push_back(Build<TCoAtom>(ctx, pos).Value(label).Done());
            }
            auto result = Build<TDqCnResult>(ctx, pos)
                .Output().Stage(physicalStages[i].back()).Index().Build("0").Build()
                .ColumnHints().Add(columns).Build().Done().Ptr();
            TypeAnnotate(result);
            auto binding = Build<TKqpTxResultBinding>(ctx, pos)
                .Type(ExpandType(pos, *result->GetTypeAnn(), ctx))
                .TxIndex().Build(ToString(transactions.size()))
                .ResultIndex().Build(ToString(results.size())).Done().Ptr();
            if (const auto& name = Roots[i]->PlanProps.MaterializedResult) {
                Y_ENSURE(materializedBindings.emplace(*name, binding).second);
            } else {
                orderedResults.emplace_back(Roots[i]->PlanProps.ResultIndex.value_or(orderedResults.size()), binding);
            }
            results.push_back(result);
        }
        transactions.push_back(Build<TKqpPhysicalTx>(ctx, pos)
            .Stages().Add(stages).Build()
            .Results().Add(results).Build()
            .ParamBindings().Add(paramBindings).Build()
            .Settings(settings.BuildNode(ctx, pos)).Done().Ptr());
        begin = end;
    }
    std::sort(orderedResults.begin(), orderedResults.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (auto& [index, binding] : orderedResults) {
        resultBindings.push_back(std::move(binding));
    }
    return Build<TKqpPhysicalQuery>(ctx, pos)
        .Transactions().Add(transactions).Build()
        .Results().Add(resultBindings).Build()
        .Settings(GetPhysicalQuerySettings().BuildNode(ctx, pos)).Done().Ptr();
}

TKqpPhyQuerySettings TPhysicalQueryBuilder::GetPhysicalQuerySettings() const {
    auto& kqpCtx = RBOCtx.KqpCtx;
    TKqpPhyQuerySettings querySettings;
    switch (kqpCtx.QueryCtx->Type) {
        case EKikimrQueryType::Dml: {
            querySettings.Type = EPhysicalQueryType::Data;
            break;
        }
        case EKikimrQueryType::Query: {
            querySettings.Type = EPhysicalQueryType::GenericQuery;
            break;
        }
        case EKikimrQueryType::Scan: {
            querySettings.Type = EPhysicalQueryType::Scan;
            break;
        }
        default: {
            // Should fallback to old pipeline.
            YQL_ENSURE(false, "Unsupported query type for NEW RBO " << kqpCtx.QueryCtx->Type);
        }
    }
    return querySettings;
}

TKqpPhyTxSettings TPhysicalQueryBuilder::GetPhysicalTxSettings(size_t rootIdx) const {
    auto& kqpCtx = RBOCtx.KqpCtx;
    TKqpPhyTxSettings txSettings;
    const bool withEffects = Roots.at(rootIdx)->PlanProps.WithEffects;
    txSettings.WithEffects = withEffects;

    switch (kqpCtx.QueryCtx->Type) {
        case EKikimrQueryType::Dml: {
            txSettings.Type = withEffects ? EPhysicalTxType::Data : EPhysicalTxType::Compute;
            break;
        }
        case EKikimrQueryType::Query: {
            txSettings.Type = EPhysicalTxType::Generic;
            break;
        }
        case EKikimrQueryType::Scan: {
            txSettings.Type = EPhysicalTxType::Scan;
            break;
        }
        default: {
            YQL_ENSURE(false, "Unsupported tx type for NEW RBO " << kqpCtx.QueryCtx->Type);
        }
    }
    return txSettings;
}

TExprNode::TPtr TPhysicalQueryBuilder::BuildDqPhyStage(const TVector<TExprNode::TPtr>& inputs, const TVector<TExprNode::TPtr>& args,
                                                       TExprNode::TPtr physicalStageBody, NNodes::TCoNameValueTupleList&& settings, TExprContext& ctx,
                                                       TPositionHandle pos) const {
    // clang-format off
    return Build<TDqPhyStage>(ctx, pos)
        .Inputs()
            .Add(inputs)
        .Build()
        .Program()
            .Args(args)
            .Body(physicalStageBody)
        .Build()
        .Settings(settings)
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalQueryBuilder::BuildDqPhySinkStage(const TVector<TExprNode::TPtr>& inputs, const TVector<TExprNode::TPtr>& args,
                                                       TExprNode::TPtr physicalStageBody, NNodes::TCoNameValueTupleList&& settings, 
                                                       const TExprNode::TPtr& sinkSettings, const TOpTableEffect& effect,
                                                       TExprContext& ctx, TPositionHandle pos) {
    if (!effect.GetReturningColumns().Items().empty()) {
        auto stage = BuildDqPhyStage(inputs, args, physicalStageBody, std::move(settings), ctx, pos);
        TypeAnnotate(stage);
        TVector<const TItemExprType*> items;
        for (const auto& [id, name] : effect.GetReturningColumns().Items()) {
            const auto* item = ExportNativeType(effect.GetNativeIUType(id), ctx);
            items.push_back(ctx.MakeType<TItemExprType>(name, item));
        }
        const auto transform = Build<TDqTransform>(ctx, pos)
            .Index().Build("0")
            .DataSink<TKqpTableSink>().Category().Build("KqpTableSink").Cluster().Build("db").Build()
            .Type().Build("ReturningSink")
            .InputType(ExpandType(pos, GetSeqItemType(*TDqPhyStage(stage).Program().Body().Ref().GetTypeAnn()), ctx))
            .OutputType(ExpandType(pos, *ctx.MakeType<TStructExprType>(items), ctx))
            .Settings(sinkSettings)
            .Done();
        // ReturningSink performs the write and emits its result. A separate
        // sink or a copy of the input stream would duplicate effects or expose
        // rows that were rejected by a conflict/constraint check.
        return Build<TDqPhyStage>(ctx, pos).InitFrom(TDqPhyStage(stage))
            .Outputs().Add(transform).Build().Done().Ptr();
    }

    TVector<TExprNode::TPtr> outputs;
    auto dataSink = ctx.NewCallable(pos, "DataSink", {ctx.NewAtom(pos, "KqpTableSink"), ctx.NewAtom(pos, "db")});
    outputs.push_back(Build<TDqSink>(ctx, pos)
                        .Index().Value("0").Build()
                        .DataSink(dataSink)
                        .Settings(sinkSettings)
                        .Done().Ptr()
    );

    // clang-format off
    return Build<TDqPhyStage>(ctx, pos)
        .Inputs()
            .Add(inputs)
        .Build()
        .Program()
            .Args(args)
            .Body(physicalStageBody)
        .Build()
        .Settings(settings)
        .Outputs()
            .Add(outputs)
        .Build()
    .Done().Ptr();
    // clang-format on
}

void TPhysicalQueryBuilder::TopologicalSort(TDqPhyStage& dqStage, TVector<TExprNode::TPtr>& result, THashSet<const TExprNode*>& visited) const {
    visited.insert(dqStage.Raw());

    for (const auto& item : dqStage.Inputs()) {
        auto maybeConnection = item.Maybe<TDqConnection>();
        // DataSource stage as input.
        if (!maybeConnection) {
            continue;
        }

        TDqPhyStage inputStage = maybeConnection.Cast().Output().Stage().Cast<TDqPhyStage>();
        if (!visited.contains(inputStage.Raw())) {
            TopologicalSort(inputStage, result, visited);
        }
    }

    result.push_back(dqStage.Ptr());
}

void TPhysicalQueryBuilder::TopologicalSort(TDqPhyStage&& dqStage, TVector<TExprNode::TPtr>& result) const {
    THashSet<const TExprNode*> visited;
    TopologicalSort(dqStage, result, visited);
}

TVector<TExprNode::TPtr> TPhysicalQueryBuilder::PreparePhysicalStages(TVector<TExprNode::TPtr>&& physicalStages) {
    Y_ENSURE(physicalStages.size());
    auto& ctx = RBOCtx.ExprCtx;
    const bool wideChannels = RBOCtx.KqpCtx.Config->GetEnableNewRBOPhysicalStagePeephole();

    TNodeOnNodeOwnedMap replaces;
    TExprNode::TPtr rootStage;
    for (auto& stage : physicalStages) {
        auto dqPhyStage = TDqPhyStage(stage);
        // clang-format off
        auto newStage = Build<TDqPhyStage>(ctx, stage->Pos())
            .Inputs(ctx.ReplaceNodes(dqPhyStage.Inputs().Ptr(), replaces))
            .Program(dqPhyStage.Program())
            .Settings(dqPhyStage.Settings())
            .Outputs(dqPhyStage.Outputs())
        .Done().Ptr();
        // clang-format on

        if (wideChannels) {
            TypeAnnotate(newStage);
            // This helper constructs channel pack/unpack adapters only.
            // Scalar normalization and YQL peepholes do not participate.
            rootStage = NDq::RebuildStageInputsAsWide(TDqPhyStage(newStage), ctx).Ptr();
        } else {
            rootStage = newStage;
        }

        replaces[dqPhyStage.Raw()] = rootStage;
    }

    TypeAnnotate(rootStage);
    if (wideChannels) {
        rootStage = LowerPhysicalLayouts(rootStage, ctx, RBOCtx.TypeCtx);
        TypeAnnotate(rootStage);
    }
    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical stages] " << KqpExprToPrettyString(TExprBase(rootStage), ctx);

    TVector<TExprNode::TPtr> stagesTopSorted;
    TopologicalSort(TDqPhyStage(rootStage), stagesTopSorted);
    if (wideChannels && RBOCtx.KqpCtx.Config->GetBlockChannelsMode() ==
        NKikimrConfig::TTableServiceConfig_EBlockChannelsMode_BLOCK_CHANNELS_AUTO) {
        return PrepareBlockChannels(std::move(stagesTopSorted));
    }
    return stagesTopSorted;
}

TVector<TExprNode::TPtr> TPhysicalQueryBuilder::PrepareBlockChannels(TVector<TExprNode::TPtr>&& physicalStages) {
    auto& ctx = RBOCtx.ExprCtx;
    if (!RBOCtx.TypeCtx.ArrowResolver) return std::move(physicalStages);

    // A shared producer must have the same layout for every consumer. Retain
    // the existing HashV1 restriction for consumers with multiple such inputs.
    THashSet<const TExprNode*> scalarOutputs{physicalStages.back().Get()};
    for (const auto& node : physicalStages) {
        const auto stage = TDqPhyStage(node);
        size_t hashV1Inputs = 0;
        for (const auto input : stage.Inputs()) {
            if (const auto shuffle = input.Maybe<TDqCnHashShuffle>()) {
                const auto function = shuffle.Cast().HashFunc().IsValid()
                    ? FromString<NDq::EHashShuffleFuncType>(shuffle.Cast().HashFunc().Cast().StringValue())
                    : RBOCtx.KqpCtx.Config->GetDqDefaultHashShuffleFuncType();
                hashV1Inputs += function == NDq::EHashShuffleFuncType::HashV1;
            }
        }
        for (const auto input : stage.Inputs()) {
            if (const auto connection = input.Maybe<TDqConnection>()) {
                const auto output = connection.Cast().Output();
                const bool supported = input.Maybe<TDqCnHashShuffle>() || input.Maybe<TDqCnMerge>() ||
                    input.Maybe<TDqCnUnionAll>() || input.Maybe<TDqCnParallelUnionAll>() ||
                    input.Maybe<TDqCnBroadcast>() || input.Maybe<TDqCnMap>();
                if (hashV1Inputs > 1 || !supported || output.Index().Value() != "0") {
                    scalarOutputs.insert(output.Stage().Raw());
                }
            }
        }
    }

    THashMap<const TExprNode*, TExprNode::TPtr> stages;
    THashSet<const TExprNode*> blockOutputs;
    TVector<TExprNode::TPtr> result;
    for (const auto& node : physicalStages) {
        const auto stage = TDqPhyStage(node);
        TExprNode::TListType inputs, arguments;
        TNodeOnNodeOwnedMap unpackInputs;
        bool changed = false;
        for (size_t i = 0; i < stage.Inputs().Size(); ++i) {
            const auto input = stage.Inputs().Item(i);
            const auto argument = stage.Program().Args().Arg(i);
            auto newInput = input.Ptr();
            auto newArgument = argument.Ptr();
            if (const auto connection = input.Maybe<TDqConnection>()) {
                const auto output = connection.Cast().Output();
                const auto* producer = output.Stage().Raw();
                if (stages.at(producer) != output.Stage().Ptr()) {
                    auto newOutput = ctx.ChangeChild(output.Ref(), TDqOutput::idx_Stage, TExprNode::TPtr(stages.at(producer)));
                    newInput = ctx.ChangeChild(input.Ref(), TDqConnection::idx_Output, std::move(newOutput));
                    changed = true;
                }
                if (blockOutputs.contains(producer)) {
                    newArgument = ctx.NewArgument(argument.Pos(), argument.Name());
                    unpackInputs.emplace(argument.Raw(), ctx.NewCallable(argument.Pos(), "WideFromBlocks", {newArgument}));
                }
            }
            inputs.push_back(std::move(newInput));
            arguments.push_back(std::move(newArgument));
        }
        auto body = stage.Program().Body().Ptr();
        if (!unpackInputs.empty()) body = LowerPhysicalLayouts(body, ctx, RBOCtx.TypeCtx, unpackInputs);
        const auto settings = NDq::TDqStageSettings::Parse(stage);
        if (!scalarOutputs.contains(stage.Raw()) && settings.WideChannels && settings.OutputNarrowType &&
            body->IsCallable("WideFromBlocks")) {
            TVector<const TTypeAnnotationNode*> columns;
            for (const auto* column : settings.OutputNarrowType->GetItems()) columns.push_back(column->GetItemType());
            const auto supported = RBOCtx.TypeCtx.ArrowResolver->AreTypesSupported(ctx.GetPosition(stage.Pos()), columns, ctx);
            YQL_ENSURE(supported != IArrowResolver::ERROR);
            if (supported == IArrowResolver::OK) {
                body = body->HeadPtr();
                blockOutputs.insert(stage.Raw());
            }
        }
        if (!changed && body == stage.Program().Body().Ptr()) {
            stages.emplace(stage.Raw(), node);
            result.push_back(node);
            continue;
        }
        auto program = ctx.NewLambda(stage.Program().Pos(), ctx.NewArguments(stage.Program().Pos(), std::move(arguments)), std::move(body));
        auto rebuilt = Build<TDqPhyStage>(ctx, stage.Pos()).InitFrom(stage)
            .Inputs(ctx.NewList(stage.Inputs().Pos(), std::move(inputs))).Program(program).Done().Ptr();
        TypeAnnotate(rebuilt);
        stages.emplace(stage.Raw(), rebuilt);
        result.push_back(std::move(rebuilt));
    }
    return result;
}

void TPhysicalQueryBuilder::TypeAnnotate(TExprNode::TPtr& input) {
    RBOCtx.BackendTypeAnnTransformer.Rewind();
    TExprNode::TPtr output;
    IGraphTransformer::TStatus status(IGraphTransformer::TStatus::Ok);
    do {
        status = RBOCtx.BackendTypeAnnTransformer.Transform(input, output, RBOCtx.ExprCtx);
        if (output) input = output;
    } while (status == IGraphTransformer::TStatus::Repeat);

    if (status != IGraphTransformer::TStatus::Ok) {
        RBOCtx.ExprCtx.AddError(TIssue(RBOCtx.ExprCtx.GetPosition(input->Pos()), "Type inference failed for stage in NEW RBO"));
        Y_ENSURE(false);
    }

    input = output;
}

} // namespace NKikimr::NKqp
