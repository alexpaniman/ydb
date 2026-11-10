#include "kqp_host_impl.h"

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/common/kqp_user_request_context.h>
#include <ydb/core/kqp/opt/kqp_column_statistics_requester.h>
#include <ydb/core/kqp/opt/kqp_constant_folding_transformer.h>
#include <ydb/core/kqp/opt/kqp_opt.h>
#include <ydb/core/kqp/opt/kqp_opt_hash_func_propagate_transformer.h>
#include <ydb/core/kqp/opt/kqp_query_plan.h>
#include <ydb/core/kqp/opt/kqp_statistics_transformer.h>
#include <ydb/core/kqp/opt/logical/kqp_opt_cbo.h>
#include <ydb/core/kqp/opt/logical/kqp_opt_log.h>
#include <ydb/core/kqp/opt/peephole/kqp_opt_peephole.h>
#include <ydb/core/kqp/opt/physical/kqp_opt_phy.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_transformer.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider_impl.h>
#include <ydb/core/kqp/query_compiler/kqp_query_compiler.h>
#include <ydb/library/yql/providers/dq/common/yql_dq_settings.h>
#include <ydb/library/yql/providers/dq/opt/dqs_opt.h>

#include <yql/essentials/core/peephole_opt/yql_opt_peephole_physical.h>
#include <yql/essentials/core/services/yql_transform_pipeline.h>
#include <yql/essentials/core/type_ann/type_ann_expr.h>
#include <yql/essentials/core/yql_gc_transformer.h>
#include <yql/essentials/core/yql_graph_transformer.h>
#include <yql/essentials/core/yql_opt_proposed_by_data.h>
#include <yql/essentials/providers/common/transform/yql_visit.h>
#include <yql/essentials/utils/log/log.h>

#include <util/generic/is_in.h>

#include <optional>

namespace NKikimr::NKqp {

using namespace NOpt;
using namespace NYql;
using namespace NYql::NCommon;
using namespace NYql::NNodes;
using namespace NThreading;

namespace {

class TRboGcTransformer final : public TSyncTransformerBase {
public:
    TRboGcTransformer()
        : Inner(CreateGcNodeTransformer())
    {
    }

    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) override {
        return Inner->Transform(input, output, ctx);
    }

    void Rewind() override {
        // RBO rewinds annotation for each lambda in the same expression context.
        // Keep the GC allocation watermark across those calls. PrepareQueryInternal
        // creates a new pipeline (and GC transformer) for each query.
    }

private:
    TAutoPtr<IGraphTransformer> Inner;
};

TAutoPtr<IGraphTransformer> Log(const TStringBuf& transformerName, NYql::NLog::ELevel level = NYql::NLog::ELevel::TRACE) {
    return TLogExprTransformer::Sync(TStringBuilder() << transformerName << "Transformer",
        NYql::NLog::EComponent::ProviderKqp,
        level);
}

TTransformStage LogStage(const TStringBuf& transformerName, NYql::NLog::ELevel level = NYql::NLog::ELevel::TRACE) {
    return TTransformStage{ Log(transformerName, level), TStringBuilder() << "Log" << transformerName, TIssuesIds::DEFAULT_ERROR };
}

class TCompilePhysicalQueryTransformer : public TSyncTransformerBase {
public:
    TCompilePhysicalQueryTransformer(
        const TString& cluster,
        const TString& database,
        TKqlTransformContext& transformCtx,
        TKqpOptimizeContext& optimizeCtx,
        TTypeAnnotationContext& typesCtx,
        const NMiniKQL::IFunctionRegistry& funcRegistry,
        const TKikimrConfiguration::TPtr& config,
        std::optional<TQueryExecutionMetadata> nativeMetadata = std::nullopt
    )
        : Cluster(cluster)
        , Database(database)
        , TransformCtx(transformCtx)
        , OptimizeCtx(optimizeCtx)
        , TypesCtx(typesCtx)
        , FuncRegistry(funcRegistry)
        , Config(config)
        , NativeMetadata(std::move(nativeMetadata))
    {
    }

    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) override {
        output = input;

        if (!TransformerFinished) {
            TransformerFinished = true;
            auto& preparedQuery = *TransformCtx.QueryCtx->PreparingQuery;
            TKqpPhysicalQuery physicalQuery(input);

            YQL_ENSURE(NativeMetadata || TransformCtx.DataQueryBlocks);
            auto compiler = CreateKqpQueryCompiler(Cluster, Database, FuncRegistry, TypesCtx, OptimizeCtx, Config);
            if (!ctx.IssueManager.GetIssues().Empty()) {
                bool errorFound = false;
                bool wrongIndexUsageFound = false;
                const auto& issues = ctx.IssueManager.GetIssues();
                for(const auto& issue: issues) {
                    WalkThroughIssues(issue, false, [&errorFound, &wrongIndexUsageFound](const NYql::TIssue& issue, int level) {
                        Y_UNUSED(level);
                        if (issue.GetSeverity() == NYql::TSeverityIds::S_FATAL || issue.GetSeverity() == NYql::TSeverityIds::S_ERROR) {
                            errorFound = true;
                        }

                        if (issue.GetCode() == EYqlIssueCode::TIssuesIds_EIssueCode_KIKIMR_WRONG_INDEX_USAGE) {
                            wrongIndexUsageFound = true;
                        }
                    });
                }

                if (errorFound && wrongIndexUsageFound) {
                    ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), "Failed to compile physical query."));
                    return TStatus::Error;
                }
            }

            auto ret = NativeMetadata
                ? compiler->CompilePhysicalQuery(physicalQuery, *NativeMetadata, *preparedQuery.MutablePhysicalQuery(), ctx)
                : compiler->CompilePhysicalQuery(physicalQuery, *TransformCtx.DataQueryBlocks, *preparedQuery.MutablePhysicalQuery(), ctx);
            if (!ret) {
                ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), "Failed to compile physical query."));
                return TStatus::Error;
            }

            preparedQuery.SetVersion(NKikimrKqp::TPreparedQuery::VERSION_PHYSICAL_V1);
        }

        return TStatus::Ok;
    }

    void Rewind() override {
        TransformerFinished = false;
    }

private:
    const TString Cluster;
    const TString Database;
    TKqlTransformContext& TransformCtx;
    TKqpOptimizeContext& OptimizeCtx;
    TTypeAnnotationContext& TypesCtx;
    const NMiniKQL::IFunctionRegistry& FuncRegistry;
    TKikimrConfiguration::TPtr Config;
    const std::optional<TQueryExecutionMetadata> NativeMetadata;
    bool TransformerFinished = false;
};

class TPrepareQueryAsyncResult : public TKqpAsyncResultBase<IKikimrQueryExecutor::TQueryResult, false> {
public:
    using TResult = IKikimrQueryExecutor::TQueryResult;

    TPrepareQueryAsyncResult(
        const TExprNode::TPtr& queryRoot,
        IGraphTransformer& transformer,
        TExprContext& ctx,
        TKqlTransformContext& transformCtx)
        : TKqpAsyncResultBase(queryRoot, ctx, transformer, nullptr)
        , TransformCtx(transformCtx)
    {
    }

    void FillResult(TResult& queryResult) const override {
        queryResult.QueryStats.CopyFrom(TransformCtx.QueryStats);
    }

private:
    TKqlTransformContext& TransformCtx;
};

class TKqpRunner : public IKqpRunner {
public:
    TKqpRunner(TIntrusivePtr<IKqpGateway> gateway, const TString& cluster,
        const TIntrusivePtr<TTypeAnnotationContext>& typesCtx, const TIntrusivePtr<TKikimrSessionContext>& sessionCtx,
        const TIntrusivePtr<TKqlTransformContext>& transformCtx, const NMiniKQL::IFunctionRegistry& funcRegistry,
        TActorSystem* actorSystem, bool usePessimisticLocks = false)
        : Gateway(gateway)
        , Cluster(cluster)
        , TypesCtx(*typesCtx)
        , SessionCtx(sessionCtx)
        , FunctionRegistry(funcRegistry)
        , Config(sessionCtx->ConfigPtr())
        , TransformCtx(transformCtx)
        , OptimizeCtx(MakeIntrusive<TKqpOptimizeContext>(cluster, Config, sessionCtx->QueryPtr(),
            sessionCtx->TablesPtr(), sessionCtx->GetUserRequestContext(), usePessimisticLocks))
        , BuildQueryCtx(MakeIntrusive<TKqpBuildQueryContext>())
        , Pctx(TKqpProviderContext(*OptimizeCtx,
            Config->CostBasedOptimizationLevel.Get().GetOrElse(Config->GetDefaultCostBasedOptimizationLevel()),
            Config->UseBlockHashJoin.Get().GetOrElse(false),
            Config))
        , ActorSystem(actorSystem)
    {
        sessionCtx->SetInternalTypeAnnTransformer(CreateKqpTypeAnnotationTransformer(Cluster, sessionCtx->TablesPtr(), Config));
    }

    TIntrusivePtr<TAsyncQueryResult> PrepareNativeQuery(const TString& cluster,
        TNativeQuery query, TExprContext& ctx,
        const IKikimrQueryExecutor::TExecuteSettings& settings) override
    {
        auto& roots = query.Roots;
        YQL_ENSURE(cluster == Cluster);
        YQL_ENSURE(!settings.CommitTx && !settings.RollbackTx);
        YQL_ENSURE(TransformCtx->QueryCtx->PrepareOnly && TransformCtx->QueryCtx->PreparingQuery);
        YQL_ENSURE(!roots.empty());

        const auto queryType = TransformCtx->QueryCtx->Type;
        if (!IsIn({EKikimrQueryType::Dml, EKikimrQueryType::Scan, EKikimrQueryType::Query}, queryType)) {
            ctx.AddError(TIssue(ctx.GetPosition(roots.front()->Pos), "Native frontend supports data, scan and generic queries"));
            return MakeKikimrResultHolder(ResultFromErrors<IKqpHost::TQueryResult>(ctx.IssueManager.GetIssues()));
        }
        if (queryType == EKikimrQueryType::Scan && roots.size() != 1) {
            ctx.AddError(TIssue(ctx.GetPosition(roots.front()->Pos), "Scan query should have a single result set"));
            return MakeKikimrResultHolder(ResultFromErrors<IKqpHost::TQueryResult>(ctx.IssueManager.GetIssues()));
        }

        YQL_ENSURE(query.Execution.DiscardResults.size() == roots.size());
        bool isOlap = false;
        for (const auto& root : roots) {
            YQL_ENSURE(root && root->PlanProps.NativeExpressions && !root->PlanProps.WithEffects);
            for (const auto& item : IterateSubtreeWithSubplans(root.Get(), root->PlanProps)) {
                if (item.Current->Kind == EOperator::Source &&
                    CastOperator<TOpRead>(*item.Current).GetTableStorageType() == NYql::EStorageType::ColumnStorage)
                    isOlap = true;
            }
        }
        // Apply the same execution setting at both frontend boundaries. RBO
        // rules still decide when a particular plan must disable blocks.
        if (queryType == EKikimrQueryType::Query) ConfigureBlockEngine(isOlap);
        if (queryType == EKikimrQueryType::Dml) {
            auto& prepared = *TransformCtx->QueryCtx->PreparingQuery;
            for (ui32 i = 0; i < roots.size(); ++i) {
                auto& result = *prepared.AddResults();
                result.SetKqlIndex(0);
                result.SetResultIndex(i);
                result.SetRowsLimit(Config->_ResultRowsLimit.Get().GetOrElse(0));
                for (const auto& [id, name] : roots[i]->GetColumns().Items()) {
                    Y_UNUSED(id);
                    *result.AddColumnHints() = name;
                }
            }
        }

        TransformCtx->Reset();
        BuildQueryCtx->Reset();
        auto typeAnnotation = TTransformationPipeline(&TypesCtx)
            .Add(TAutoPtr<IGraphTransformer>(new TRboGcTransformer()), "GC")
            .AddTypeAnnotationTransformer()
            .Build(false);
        auto* rbo = new TKqpNewRBOTransformer(OptimizeCtx, TypesCtx, std::move(typeAnnotation),
            SessionCtx->Tables(), Cluster, SessionCtx->GetDatabase(), ActorSystem, FunctionRegistry, TransformCtx);
        const auto pos = roots.front()->Pos;
        rbo->SetNativeRoots(std::move(roots));
        TAutoPtr<IGraphTransformer> nativeRbo(rbo);
        TAutoPtr<IGraphTransformer> compile(new TCompilePhysicalQueryTransformer(Cluster,
            SessionCtx->GetDatabase(), *TransformCtx, *OptimizeCtx, TypesCtx, FunctionRegistry, Config,
            std::move(query.Execution)));
        // RBO emits fresh transaction/result-binding nodes. The legacy entry
        // re-enters its surrounding annotation pipeline after that rewrite;
        // native entry performs this backend-only step explicitly.
        auto physicalTypes = TTransformationPipeline(&TypesCtx)
            .AddServiceTransformers()
            .AddTypeAnnotationTransformer()
            .AddPostTypeAnnotation(/*forSubGraph=*/true)
            .Build(false);
        NativeTransformer = CreateCompositeGraphTransformer({
            TTransformStage{nativeRbo, "NativeRBO", TIssuesIds::DEFAULT_ERROR},
            TTransformStage{CreateKqpRBOCleanupTransformer(TypesCtx), "NativeRBOPhysicalQuery", TIssuesIds::DEFAULT_ERROR},
            TTransformStage{physicalTypes, "NativePhysicalTypes", TIssuesIds::DEFAULT_ERROR},
            TTransformStage{CreateSaveExplainTransformerInput(*TransformCtx), "NativeSaveExplainInput", TIssuesIds::DEFAULT_ERROR},
            TTransformStage{compile, "CompileNativePhysicalQuery", TIssuesIds::DEFAULT_ERROR},
            TTransformStage{CreateKqpRBOExplainPreparedTransformer(Gateway, Cluster, TransformCtx,
                &FunctionRegistry, TypesCtx, OptimizeCtx), "ExplainNativeQuery", TIssuesIds::DEFAULT_ERROR}
        }, false);
        NativeTransformer->Rewind();

        // Graph-transform scheduling needs a handle; it carries no query AST.
        return MakeIntrusive<TPrepareQueryAsyncResult>(ctx.NewWorld(pos), *NativeTransformer, ctx, *TransformCtx);
    }

    TIntrusivePtr<TAsyncQueryResult> PrepareDataQuery(const TString& cluster, const TExprNode::TPtr& query,
        TExprContext& ctx, const IKikimrQueryExecutor::TExecuteSettings& settings) override
    {
        YQL_ENSURE(TransformCtx->QueryCtx->Type == EKikimrQueryType::Dml);
        YQL_ENSURE(TMaybeNode<TKiDataQueryBlocks>(query));

        YQL_CLOG(DEBUG, CoreDq) << "Before any rewrites: " << KqpExprToPrettyString(*query, ctx);

        return PrepareQueryInternal(cluster, TKiDataQueryBlocks(query), ctx, settings);
    }

    TIntrusivePtr<TAsyncQueryResult> PrepareScanQuery(const TString& cluster, const TExprNode::TPtr& query,
        TExprContext& ctx, const IKikimrQueryExecutor::TExecuteSettings& settings) override
    {
        YQL_ENSURE(TransformCtx->QueryCtx->Type == EKikimrQueryType::Scan);
        YQL_ENSURE(TMaybeNode<TKiDataQueryBlocks>(query));

        TKiDataQueryBlocks dataQueryBlocks(query);

        if (dataQueryBlocks.ArgCount() != 1) {
            ctx.AddError(YqlIssue(ctx.GetPosition(dataQueryBlocks.Pos()), TIssuesIds::KIKIMR_PRECONDITION_FAILED,
               "Scan query should have single query block."));
            return MakeKikimrResultHolder(ResultFromErrors<IKqpHost::TQueryResult>(ctx.IssueManager.GetIssues()));
        }

        const auto& queryBlock = dataQueryBlocks.Arg(0);
        if (queryBlock.Results().Size() != 1) {
            ctx.AddError(YqlIssue(ctx.GetPosition(dataQueryBlocks.Pos()), TIssuesIds::KIKIMR_PRECONDITION_FAILED,
                "Scan query should have a single result set."));
            return MakeKikimrResultHolder(ResultFromErrors<IKqpHost::TQueryResult>(ctx.IssueManager.GetIssues()));
        }
        if (queryBlock.Effects().ArgCount() > 0) {
            ctx.AddError(YqlIssue(ctx.GetPosition(dataQueryBlocks.Pos()), TIssuesIds::KIKIMR_PRECONDITION_FAILED,
                "Scan query cannot have data modifications."));
            return MakeKikimrResultHolder(ResultFromErrors<IKqpHost::TQueryResult>(ctx.IssueManager.GetIssues()));
        }

        IKikimrQueryExecutor::TExecuteSettings scanSettings(settings);
        return PrepareQueryInternal(cluster, dataQueryBlocks, ctx, scanSettings);
    }

    TIntrusivePtr<TAsyncQueryResult> PrepareQuery(const TString& cluster, const TExprNode::TPtr& query,
        TExprContext& ctx, const IKikimrQueryExecutor::TExecuteSettings& settings) override
    {
        YQL_ENSURE(IsIn({EKikimrQueryType::Query, EKikimrQueryType::Script}, TransformCtx->QueryCtx->Type));
        YQL_ENSURE(TMaybeNode<TKiDataQueryBlocks>(query));

        YQL_CLOG(DEBUG, CoreDq) << "Before any rewrites: " << KqpExprToPrettyString(*query, ctx);

        const auto dataQueryBlocks = TKiDataQueryBlocks(query);

        ConfigureBlockEngine(IsOlapQuery(dataQueryBlocks));

        return PrepareQueryInternal(cluster, dataQueryBlocks, ctx, settings);
    }

private:
    bool IsOlapQuery(const TKiDataQueryBlocks& dataQueryBlocks) {
        if (dataQueryBlocks.ArgCount() != 1) {
            return false;
        }
        const auto& operations = dataQueryBlocks.Arg(0).Operations();
        return std::any_of(
                std::begin(operations),
                std::end(operations),
                [this](const auto& operation) {
                    if (operation.Operation() == "FillTable") {
                        // FillTable operation compiles without existing table.
                        return false;
                    }
                    const auto& tableData = SessionCtx->Tables().ExistingTable(operation.Cluster(), operation.Table());
                    return tableData.Metadata->IsOlap();
                });
    }

    void ConfigureBlockEngine(bool isOlap) {
        if (isOlap) {
            if (TransformCtx->Config->DisableBlockExecution.Get().GetOrElse(false)) {
                TypesCtx.BlockEngineMode = NYql::EBlockEngineMode::Disable;
            } else {
                switch (TransformCtx->Config->GetBlockChannelsMode()) {
                    case NKikimrConfig::TTableServiceConfig_EBlockChannelsMode_BLOCK_CHANNELS_SCALAR:
                    case NKikimrConfig::TTableServiceConfig_EBlockChannelsMode_BLOCK_CHANNELS_AUTO:
                        TypesCtx.BlockEngineMode = NYql::EBlockEngineMode::Auto;
                        break;
                    case NKikimrConfig::TTableServiceConfig_EBlockChannelsMode_BLOCK_CHANNELS_FORCE:
                        TypesCtx.BlockEngineMode = NYql::EBlockEngineMode::Force;
                        break;
                    default:
                        YQL_ENSURE(false);
                }
            }
        }
    }

    TIntrusivePtr<TAsyncQueryResult> PrepareQueryInternal(const TString& cluster,
        const TKiDataQueryBlocks& dataQueryBlocks, TExprContext& ctx,
        const IKikimrQueryExecutor::TExecuteSettings& settings)
    {
        CreateGraphTransformer(&TypesCtx, SessionCtx, FunctionRegistry);

        YQL_ENSURE(cluster == Cluster);
        YQL_ENSURE(!settings.CommitTx);
        YQL_ENSURE(!settings.RollbackTx);
        YQL_ENSURE(TransformCtx->QueryCtx->PrepareOnly);
        YQL_ENSURE(TransformCtx->QueryCtx->PreparingQuery);

        auto* queryCtx = TransformCtx->QueryCtx.Get();

        if (queryCtx->Type == EKikimrQueryType::Dml) {
            ui32 resultsCount = 0;
            for (const auto& block : dataQueryBlocks) {
                for (ui32 i = 0; i < block.Results().Size(); ++i, ++resultsCount) {
                    auto& result = *queryCtx->PreparingQuery->AddResults();
                    result.SetKqlIndex(0);
                    result.SetResultIndex(resultsCount);
                    for (const auto& column : block.Results().Item(i).Columns()) {
                        *result.AddColumnHints() = column.Value();
                    }
                    result.SetRowsLimit(FromString<ui64>(block.Results().Item(i).RowsLimit()));
                }
            }
        }

        const bool sysColumnsEnabled = TransformCtx->Config->SystemColumnsEnabled();
        auto kqlQueryBlocks = BuildKqlQuery(dataQueryBlocks, *TransformCtx->Tables, ctx, sysColumnsEnabled, OptimizeCtx, TypesCtx);
        if (!kqlQueryBlocks) {
            return MakeKikimrResultHolder(ResultFromErrors<IKqpHost::TQueryResult>(ctx.IssueManager.GetIssues()));
        }

        TExprNode::TPtr query = kqlQueryBlocks->Ptr();
        YQL_CLOG(DEBUG, ProviderKqp) << "Initial KQL query: " << KqpExprToPrettyString(*query, ctx);
        YQL_CLOG(DEBUG, CoreDq) << "Initial KQL query: " << KqpExprToPrettyString(*query, ctx);


        TransformCtx->Reset();
        BuildQueryCtx->Reset();
        Transformer->Rewind();

        TransformCtx->DataQueryBlocks = dataQueryBlocks;

        return MakeIntrusive<TPrepareQueryAsyncResult>(query, *Transformer, ctx, *TransformCtx);
    }

    void CreateGraphTransformer(const TIntrusivePtr<TTypeAnnotationContext>& typesCtx, const TIntrusivePtr<TKikimrSessionContext>& sessionCtx,
        const NMiniKQL::IFunctionRegistry& funcRegistry)
    {
        auto preparedExplainTransformer = CreateKqpExplainPreparedTransformer(
            Gateway, Cluster, TransformCtx, &funcRegistry, *typesCtx, OptimizeCtx);

        sessionCtx->SetInternalTypeAnnTransformer(CreateKqpTypeAnnotationTransformer(Cluster, sessionCtx->TablesPtr(), Config));
        auto physicalOptimizePipeline = TTransformationPipeline(typesCtx)
            .AddServiceTransformers()
            .Add(Log("PhysicalOptimize"), "LogPhysicalOptimize")
            .AddPreTypeAnnotation()
            .AddExpressionEvaluation(funcRegistry)
            .AddIOAnnotation()
            .AddTypeAnnotationTransformer()
            .Add(NOpt::CreateKqpCheckQueryTransformer(), "CheckKqlQuery")
            .AddPostTypeAnnotation(/* forSubgraph */ true)
            .AddCommonOptimization()
            .Add(CreateKqpConstantFoldingTransformer(OptimizeCtx, *typesCtx, Config), "ConstantFolding")
            .Add(CreateKqpColumnStatisticsRequester(Config, *typesCtx, SessionCtx->Tables(), Cluster, sessionCtx->GetDatabase(), ActorSystem), "ColumnStatisticsRequester")
            .Add(CreateKqpStatisticsTransformer(OptimizeCtx, *typesCtx, Config, Pctx), "Statistics")
            .Add(CreateKqpLogOptTransformer(OptimizeCtx, *typesCtx, Config), "LogicalOptimize")
            .Add(CreateLogicalDataProposalsInspector(*typesCtx), "ProvidersLogicalOptimize")
            .Add(CreateKqpPhyOptTransformer(OptimizeCtx, *typesCtx), "KqpPhysicalOptimize");

        if (sessionCtx->Config().UseBlockReader.Get().GetOrElse(false)) {
            physicalOptimizePipeline.Add(NDqs::CreateDqsRewritePhyBlockReadOnDqIntegrationTransformer(*typesCtx), "ReplaceWideReadsWithBlock");
        }

        auto physicalOptimizeTransformer = CreateKqpQueryBlocksTransformer(physicalOptimizePipeline
            .Add(CreatePhysicalDataProposalsInspector(*typesCtx), "ProvidersPhysicalOptimize")
            .Add(CreateKqpFinalizingOptTransformer(OptimizeCtx), "FinalizingOptimize")
            .Add(CreateKqpQueryPhasesTransformer(), "QueryPhases")
            .Add(CreateKqpQueryEffectsTransformer(OptimizeCtx), "QueryEffects")
            .Add(CreateKqpSinkPrecomputeTransformer(OptimizeCtx), "KqpSinkPrecompute")
            .Add(CreateKqpCheckPhysicalQueryTransformer(OptimizeCtx), "CheckKqlPhysicalQuery")
            .Build(false));

        auto physicalBuildTxsTransformer = CreateKqpQueryBlocksTransformer(TTransformationPipeline(typesCtx)
            .AddServiceTransformers()
            .Add(Log("PhysicalBuildTxs"), "LogPhysicalBuildTxs")
            .AddTypeAnnotationTransformer()
            .AddPostTypeAnnotation(/* forSubgraph */ true)
            .Add(
                CreateKqpBuildTxsTransformer(
                    OptimizeCtx,
                    BuildQueryCtx,
                    *typesCtx,
                    Config),
                "BuildPhysicalTxs")
            .Build(false));

        auto physicalBuildQueryTransformer = TTransformationPipeline(typesCtx)
            .AddServiceTransformers()
            .Add(Log("PhysicalBuildQuery"), "LogPhysicalBuildQuery")
            .AddTypeAnnotationTransformer()
            .AddPostTypeAnnotation()
            .Add(CreateKqpBuildPhysicalQueryTransformer(OptimizeCtx, BuildQueryCtx), "BuildPhysicalQuery")
            .Add(CreateKqpTxsHashFuncPropagateTransformer(*typesCtx, Config), "HashFuncPropagate")
            .Add(CreateKqpStatisticsTransformer(OptimizeCtx, *typesCtx, Config, Pctx), "Statistics")
            .Build(false);

        auto physicalPeepholeTransformer = TTransformationPipeline(typesCtx)
            .AddServiceTransformers()
            .Add(Log("PhysicalPeephole"), "LogPhysicalPeephole")
            .AddTypeAnnotationTransformer()
            .AddPostTypeAnnotation()
            .Add(GetDqIntegrationPeepholeTransformer(false, typesCtx), "DqIntegrationPeephole")
            .Add(CreateKqpTxsPeepholeTransformer(*typesCtx, Config, OptimizeCtx), "Peephole")
            .Build(false);

        TAutoPtr<IGraphTransformer> compilePhysicalQuery(new TCompilePhysicalQueryTransformer(Cluster,
            SessionCtx->GetDatabase(),
            *TransformCtx,
            *OptimizeCtx,
            *typesCtx,
            funcRegistry,
            Config));

        Transformer = CreateCompositeGraphTransformer(
            {
                TTransformStage{ physicalOptimizeTransformer, "PhysicalOptimize", TIssuesIds::DEFAULT_ERROR },
                LogStage("PhysicalOptimize"),
                TTransformStage{ physicalBuildTxsTransformer, "PhysicalBuildTxs", TIssuesIds::DEFAULT_ERROR },
                LogStage("PhysicalBuildTxs"),
                TTransformStage{ physicalBuildQueryTransformer, "PhysicalBuildQuery", TIssuesIds::DEFAULT_ERROR },
                LogStage("PhysicalBuildQuery"),
                TTransformStage{ CreateSaveExplainTransformerInput(*TransformCtx), "SaveExplainTransformerInput", TIssuesIds::DEFAULT_ERROR },
                TTransformStage{ physicalPeepholeTransformer, "PhysicalPeephole", TIssuesIds::DEFAULT_ERROR },
                LogStage("PhysicalPeephole"),
                TTransformStage{ compilePhysicalQuery, "CompilePhysicalQuery", TIssuesIds::DEFAULT_ERROR },
                TTransformStage{ preparedExplainTransformer, "ExplainQuery", TIssuesIds::DEFAULT_ERROR }, // TODO(sk): only on stats mode or if explain-only
            },
            false
        );


    }

    static bool MergeFlagValue(const TMaybe<bool>& configFlag, const TMaybe<bool>& flag) {
        if (flag) {
            return *flag;
        }

        if (configFlag) {
            return *configFlag;
        }

        return false;
    }

private:
    TIntrusivePtr<IKqpGateway> Gateway;
    TString Cluster;
    TTypeAnnotationContext& TypesCtx;
    TIntrusivePtr<TKikimrSessionContext> SessionCtx;
    const NMiniKQL::IFunctionRegistry& FunctionRegistry;
    TKikimrConfiguration::TPtr Config;

    TIntrusivePtr<TKqlTransformContext> TransformCtx;
    TIntrusivePtr<TKqpOptimizeContext> OptimizeCtx;
    TIntrusivePtr<TKqpBuildQueryContext> BuildQueryCtx;

    TKqpProviderContext Pctx;

    TAutoPtr<IGraphTransformer> Transformer;
    TAutoPtr<IGraphTransformer> NativeTransformer;

    TActorSystem* ActorSystem;
};

} // anonymous namespace

TIntrusivePtr<IKqpRunner> CreateKqpRunner(TIntrusivePtr<IKqpGateway> gateway, const TString& cluster,
    const TIntrusivePtr<TTypeAnnotationContext>& typesCtx, const TIntrusivePtr<TKikimrSessionContext>& sessionCtx,
    const TIntrusivePtr<TKqlTransformContext>& transformCtx, const NMiniKQL::IFunctionRegistry& funcRegistry, TActorSystem* actorSystem,
    bool usePessimisticLocks)
{
    return new TKqpRunner(gateway, cluster, typesCtx, sessionCtx, transformCtx, funcRegistry, actorSystem, usePessimisticLocks);
}

} // namespace NKikimr::NKqp
