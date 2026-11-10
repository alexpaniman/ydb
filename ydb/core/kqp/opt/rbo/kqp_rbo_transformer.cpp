#include "kqp_rbo_transformer.h"
#include "kqp_operator.h"
#include "kqp_rbo_rules.h"
#include "traces/kqp_rbo_trace_output.h"

#include <ydb/core/kqp/host/kqp_transform.h>

#include <util/generic/algorithm.h>
#include <util/generic/string.h>
#include <util/system/env.h>

#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/utils/log/log.h>

#include <memory>
#include <optional>
#include <utility>

namespace NKikimr::NKqp {

using namespace NYql;
using namespace NYql::NNodes;
using namespace NKikimr::NKqp;
using namespace NYql::NDq;

namespace {

NJson::TJsonValue MakeNewRBOOptimizerStats(const NOpt::TKqpOptimizeContext& kqpCtx) {
    const auto& cboStats = kqpCtx.CBOStats;

    NJson::TJsonValue optimizerStats(NJson::EJsonValueType::JSON_MAP);
    optimizerStats["CBOTreesTotal"] = cboStats.TreesTotal;
    optimizerStats["CBOTreesOptimized"] = cboStats.TreesOptimized;
    return optimizerStats;
}







} // anonymous namespace





IGraphTransformer::TStatus TKqpNewRBOTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) {

    Y_ENSURE(!Roots.empty(), "New RBO requires native query roots");
    // Backend annotation can restart the composite pipeline. Native roots
    // are consumed once: their stage assignments must never enter RBO again.
    if (NativeOptimized) return TStatus::Ok;
    for (auto& root : Roots) {
        root->ComputeParents();
    }
    if (IsSuitableToRequestStatistics()) {
        const auto status = RequestColumnStatistics(ctx);
        if (status == TStatus::Async || status == TStatus::Error) {
            return status;
        }
    }
    return ContinueOptimizations(input, output, ctx);
}

NThreading::TFuture<void> TKqpNewRBOTransformer::DoGetAsyncFuture(const TExprNode& input) {
    Y_UNUSED(input);
    return ColumnStatisticsReadiness;
}

bool TKqpNewRBOTransformer::IsSuitableToCollectStatistics(IOperator* op) const {
    return op->Props.Metadata.has_value();
}

void TKqpNewRBOTransformer::CollectTablesAndColumnsNames(IOperator* op, const TColumnLineage& lineage) {
    if (MatchOperator<TOpFilter>(op)) {
        const auto& filter = CastOperator<TOpFilter>(*op);
        CollectTablesAndColumnsNames(filter.GetFilterExpression(), *filter.GetInput(), lineage);
    } else if (MatchOperator<TOpJoin>(op)) {
        // Fetching statistics for join cardinality correction.
        CollectJoinKeysColumns(CastOperator<TOpJoin>(op), lineage);
    } else if (MatchOperator<TOpRead>(op)) {
        // Fetching statistics for filters already pushed down into the read.
        const auto read = CastOperator<TOpRead>(op);
        if (read->OriginalPredicate.has_value()) {
            CollectTablesAndColumnsNames(read->OriginalPredicate.value(), *read, lineage);
        }
    }
}

void TKqpNewRBOTransformer::CollectTablesAndColumnsNames(const TExpression& expr, const IOperator& input, const TColumnLineage& lineage) {
    THashMap<TString, std::pair<TVector<const TColumnLineageEntry*>, TVector<const TColumnLineageEntry*>>> keysByTablePair;
    const auto collectJoinKey = [&](TInfoUnitId lhs, TInfoUnitId rhs) {
        const auto* lhsEntry = FindSourceStatistics(input, lhs, lineage);
        const auto* rhsEntry = FindSourceStatistics(input, rhs, lineage);
        if (!lhsEntry || !rhsEntry) return;
        const auto& lhsTable = lhsEntry->TableName;
        const auto& rhsTable = rhsEntry->TableName;
        if (lhsTable.empty() || rhsTable.empty() || lhsTable == rhsTable) return;
        HistColumnsByTableName[lhsTable].insert(lhsEntry->ColumnName);
        HistColumnsByTableName[rhsTable].insert(rhsEntry->ColumnName);
        auto& keys = keysByTablePair[TStringBuilder() << lhsTable << '\0' << rhsTable];
        keys.first.push_back(lhsEntry);
        keys.second.push_back(rhsEntry);
    };

        const auto usage = CollectNativePredicateStatistics(*expr.GetNativeArena(), expr.GetNativeRoot());
        const auto collectColumns = [&](const TUnorderedIUs& columns, auto& requests) {
            for (const auto column : columns) {
                const auto* entry = FindSourceStatistics(input, column, lineage);
                if (entry && !entry->TableName.empty()) requests[entry->TableName].insert(entry->ColumnName);
            }
        };
        collectColumns(usage.CountMinColumns, CMColumnsByTableName);
        collectColumns(usage.HistogramColumns, HistColumnsByTableName);
        for (const auto& [lhs, rhs] : usage.JoinKeys) collectJoinKey(lhs, rhs);


    for (const auto& [_, keys] : keysByTablePair) {
        CollectJoinKeysTuple(keys.first);
        CollectJoinKeysTuple(keys.second);
    }
}

void TKqpNewRBOTransformer::CollectJoinKeysColumns(TOpJoin* join, const TColumnLineage& lineage) {
    // For join cardinality correction, only the equi-width histogram of both join-key columns are needed.
    // Keys of a side the join does not output are skipped.
    const auto& outputIUs = join->GetOutputIUs();
    auto findSource = [&](TInfoUnitId key) {
        return outputIUs.Contains(key) ? FindSourceStatistics(*join, key, lineage) : nullptr;
    };
    auto requestHistogram = [&](const TColumnLineageEntry* entry) {
        if (!entry || entry->TableName == "") {
            return;
        }
        const auto& tableName = entry->TableName;
        const auto& colName = entry->ColumnName;
        HistColumnsByTableName[tableName].insert(colName);
    };

    TVector<const TColumnLineageEntry*> lhsKeys;
    TVector<const TColumnLineageEntry*> rhsKeys;
    for (const auto& [lhsKey, rhsKey, equalNulls] : join->JoinKeys.Items()) {
        lhsKeys.push_back(findSource(lhsKey));
        rhsKeys.push_back(findSource(rhsKey));
        requestHistogram(lhsKeys.back());
        requestHistogram(rhsKeys.back());
    }

    CollectJoinKeysTuple(lhsKeys);
    CollectJoinKeysTuple(rhsKeys);
}

// Each key is its source column, or null when it has none.
void TKqpNewRBOTransformer::CollectJoinKeysTuple(const TVector<const TColumnLineageEntry*>& joinKeys) {
    if (joinKeys.size() < 2) {
        return;
    }

    TString tableName;
    THashSet<TString> columns;
    for (const auto* entry : joinKeys) {
        if (!entry || entry->TableName == "") {
            return;
        }
        if (!tableName.empty() && tableName != entry->TableName) {
            return;
        }
        tableName = entry->TableName;
        columns.insert(entry->ColumnName);
    }

    if (columns.size() != joinKeys.size()) {
        return;
    }

    if (auto tuple = FindEqHeightHistogramTuple(tableName, columns)) {
        EqHeightHistTuplesByTableName[tableName].emplace(NYql::MakeMultiColumnKey(*tuple), *tuple);
    }
}

std::optional<TVector<TString>> TKqpNewRBOTransformer::FindEqHeightHistogramTuple(
    const TString& tableName,
    const THashSet<TString>& columns) const
{
    const auto& tableMeta = Tables.GetTable(Cluster, tableName).Metadata;
    if (!tableMeta) {
        return std::nullopt;
    }

    auto sameColumns = [&columns](const TVector<TString>& candidate) {
        return candidate.size() == columns.size()
            && AllOf(candidate, [&columns](const TString& column) { return columns.contains(column); });
    };

    for (const auto& description : tableMeta->MultiColumnStatistics) {
        if (Find(description.Types, "EQ_HEIGHT_HISTOGRAM") == description.Types.end()) {
            continue;
        }
        if (sameColumns(description.Columns)) {
            return description.Columns;
        }
    }

    if (sameColumns(tableMeta->KeyColumnNames)) {
        return tableMeta->KeyColumnNames;
    }

    return std::nullopt;
}

void TKqpNewRBOTransformer::CollectTablesAndColumnsNames(TExprContext& ctx) {
    TRBOContext rboCtx(KqpCtx, ctx, TypeCtx, *BackendTypeAnnTransformer.Get(), FuncRegistry);
    for (auto & root : Roots) {
        root->ComputePlanMetadata(rboCtx);
        for (const auto& it : *root) {
            if (IsSuitableToCollectStatistics(it.Current)) {
                CollectTablesAndColumnsNames(it.Current, root->PlanProps.ColumnLineage);
            }
        }
    }
}

IGraphTransformer::TStatus TKqpNewRBOTransformer::RequestColumnStatistics(TExprContext& ctx) {
    CollectTablesAndColumnsNames(ctx);

    TVector<NThreading::TFuture<TColumnStatisticsResponse>> futures;
    AddStatRequest(ActorSystem, futures, Tables, Cluster, Database, TypeCtx, NStat::EStatType::COUNT_MIN_SKETCH, CMColumnsByTableName,
                   [](const NYql::TColumnStatistics& stats) { return !!stats.CountMinSketch; });
    AddStatRequest(ActorSystem, futures, Tables, Cluster, Database, TypeCtx, NStat::EStatType::EQ_WIDTH_HISTOGRAM, HistColumnsByTableName,
                   [](const NYql::TColumnStatistics& stats) { return !!stats.EqWidthHistogramEstimator; });
    AddStatRequest(ActorSystem, futures, Tables, Cluster, Database, TypeCtx, NStat::EStatType::EQ_HEIGHT_HISTOGRAM, EqHeightHistTuplesByTableName,
                   [](const NYql::TMultiColumnStatistics& stats) { return !!stats.EqHeightHistogram; });

    if (futures.empty()) {
        return TStatus::Ok;
    }

    auto sharedState = std::make_shared<TColumnStatisticsSharedState>();
    ColumnStatisticsReadiness = NThreading::WaitAll(futures).Apply(
        [weakSharedState = std::weak_ptr{sharedState}, futures = std::move(futures)](const NThreading::TFuture<void>&) mutable {
            for (auto& fut : futures) {
                if (fut.HasException()) {
                    fut.TryRethrow();
                }

                auto newStats = fut.ExtractValue();
                auto sharedState = weakSharedState.lock();
                if (!sharedState) {
                    // parent already deleted, just return
                    return;
                }
                if (!sharedState->Response.has_value()) {
                    sharedState->Response = std::move(newStats);
                } else {
                    // merge statistics
                    for (const auto& [table, column2Stat] : newStats.ColumnStatisticsByTableName) {
                        auto& oldColumn2Stat = sharedState->Response->ColumnStatisticsByTableName[table];
                        for (const auto& [column, newStat] : column2Stat.Data) {
                            auto& oldStat = oldColumn2Stat.Data[column];
                            if (newStat.CountMinSketch) {
                                oldStat.CountMinSketch = newStat.CountMinSketch;
                            }
                            if (newStat.EqWidthHistogramEstimator) {
                                oldStat.EqWidthHistogramEstimator = newStat.EqWidthHistogramEstimator;
                            }
                            if (!newStat.Type.empty()) {
                                oldStat.Type = newStat.Type;
                            }
                            if (newStat.NumUniqueVals) {
                                oldStat.NumUniqueVals = newStat.NumUniqueVals;
                            }
                            if (newStat.HyperLogLog) {
                                oldStat.HyperLogLog = newStat.HyperLogLog;
                            }
                        }
                        for (const auto& [tuple, newStat] : column2Stat.MultiData) {
                            auto& oldStat = oldColumn2Stat.MultiData[tuple];
                            oldStat.Columns = newStat.Columns;
                            oldStat.Types = newStat.Types;
                            if (newStat.EqHeightHistogram) {
                                oldStat.EqHeightHistogram = newStat.EqHeightHistogram;
                            }
                            if (newStat.CountMinSketch) {
                                oldStat.CountMinSketch = newStat.CountMinSketch;
                            }
                        }
                    }
                }
            }
        });

    SharedState = sharedState;
    return TStatus::Async;
}

bool TKqpNewRBOTransformer::IsSuitableToRequestStatistics() {
    // Currently just checking for a flag.
    return KqpCtx.Config->FeatureFlags.GetEnableColumnStatistics();
}

IGraphTransformer::TStatus TKqpNewRBOTransformer::ContinueOptimizations(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) {

    Y_UNUSED(input);
    TRBOContext rboCtx(KqpCtx, ctx, TypeCtx, *BackendTypeAnnTransformer.Get(), FuncRegistry);
    TRBOTraceOutput traceOutput(rboCtx);
    output = RBO.Optimize(Roots, rboCtx);
    traceOutput.Flush();
    if (!output) {
        return TStatus::Error;
    }
    AddPlans(rboCtx.ExecutionJson, rboCtx.ExplainJson);
    NativeOptimized = true;
    return TStatus::Ok;
}

void TKqpNewRBOTransformer::ApplyColumnStatistics() {
    Y_ENSURE(ColumnStatisticsReadiness.IsReady());
    if (!SharedState->Response->Issues().Empty()) {
        TStringStream ss;
        SharedState->Response->Issues().PrintTo(ss);
        YQL_CLOG(TRACE, ProviderKikimr) << "Can't load columns statistics for request: " << ss.Str();
    } else {
        for (auto&& [tableName, columnStatistics] : SharedState->Response->ColumnStatisticsByTableName) {
            TypeCtx.ColumnStatisticsByTableName.insert({std::move(tableName), new NYql::TOptimizerStatistics::TColumnStatMap(std::move(columnStatistics))});
        }
    }
}

IGraphTransformer::TStatus TKqpNewRBOTransformer::DoApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) {
    ApplyColumnStatistics();
    return ContinueOptimizations(input, output, ctx);
}

//FIXME: We currently support only a single plan, throw an exception if that's not the case
void TKqpNewRBOTransformer::AddPlans(std::optional<NJson::TJsonValue> execPlan, std::optional<NJson::TJsonValue> explainPlan) {
    if (!execPlan.has_value() || !explainPlan.has_value()) {
        Y_ENSURE(false, "Explain plan wasn't computed in the optimizer");
    }

    Y_ENSURE(!TransformCtx->PlanJson.has_value(), "Only a single explain is supported");

    auto planJson = NJson::TJsonValue(NJson::EJsonValueType::JSON_MAP);
    auto plans = NJson::TJsonValue(NJson::EJsonValueType::JSON_ARRAY);
    plans.AppendValue(execPlan.value());
    planJson["Plans"] = plans;
    planJson["SimplifiedPlan"] = explainPlan.value();
    planJson["SimplifiedPlan"]["OptimizerStats"] = MakeNewRBOOptimizerStats(KqpCtx);

    TransformCtx->PlanJson = planJson;
}

void TKqpNewRBOTransformer::SetNativeRoots(TVector<TIntrusivePtr<TOpRoot>> roots) {
    Y_ENSURE(Roots.empty() && !roots.empty(), "Native roots must be supplied once before optimization");
    for (const auto& root : roots) {
        Y_ENSURE(root && root->PlanProps.NativeExpressions, "Expected a native RBO root");
    }
    Roots = std::move(roots);
}

void TKqpNewRBOTransformer::Rewind() {
}

IGraphTransformer::TStatus TKqpRBOCleanupTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr &output, TExprContext &ctx) {
    TOptimizeExprSettings settings(&TypeCtx);
    Y_UNUSED(ctx);
    YQL_CLOG(TRACE, CoreDq) << "Cleanup input plan: " << KqpExprToPrettyString(TExprBase(input), ctx) << Endl;

    // We just need to find a physical query callable.
    auto physicalQueries = FindNodes(input, [](const TExprNode::TPtr& node) { return TKqpPhysicalQuery::Match(node.Get()); });
    if (physicalQueries.size() == 1) {
        output = physicalQueries.front();
        return IGraphTransformer::TStatus::Ok;
    }

    return IGraphTransformer::TStatus::Error;
}

TKqpNewRBOTransformer::TKqpNewRBOTransformer(TIntrusivePtr<TKqpOptimizeContext>& kqpCtx, TTypeAnnotationContext& typeCtx,
                                             TAutoPtr<IGraphTransformer>&& backendTypeAnnTransformer,
                                             TKikimrTablesData& tables, const TString& cluster, const TString& database, TActorSystem* actorSystem,
                                             const NMiniKQL::IFunctionRegistry& funcRegistry, TIntrusivePtr<TKqlTransformContext> transformCtx)
    : TypeCtx(typeCtx)
    , KqpCtx(*kqpCtx)
    , BackendTypeAnnTransformer(std::move(backendTypeAnnTransformer))
    , FuncRegistry(funcRegistry)
    , TransformCtx(transformCtx)
    , Tables(tables)
    , Cluster(cluster)
    , Database(database)
    , ActorSystem(actorSystem) {
    // Finally initializes all RBO optimization stages.
    InitializeRBOOptimizationStages();
}

void TKqpNewRBOTransformer::InitializeRBOOptimizationStages() {
    const bool pruneKeyColumns = KqpCtx.Config->GetEnablePruneKeyColumns();
    // Match the compiler's AllowWithSpilling setting: shared stage outputs need
    // channel spilling, otherwise every consumer needs its own producer.
    const bool allowChannelSpilling = KqpCtx.Config->GetEnableQueryServiceSpilling()
        && (KqpCtx.IsGenericQuery() || KqpCtx.IsScanQuery()) && KqpCtx.Config->SpillingEnabled();

    // Prune unused outputs before any rules that require type information.
    RBO.AddStage(std::make_unique<TGlobalPruningStage>("Early pruning"));

    // Expand aggregation.
    TVector<std::unique_ptr<IRule>> expandAggregationRules;
    expandAggregationRules.emplace_back(std::make_unique<TExpandGroupingSetsRule>());
    expandAggregationRules.emplace_back(std::make_unique<TExpandDistinctAggregationRule>());
    expandAggregationRules.emplace_back(std::make_unique<TExpandWholePartitionWindowRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Expand aggregation", std::move(expandAggregationRules)));

    // Rewrite all right joins into left joins
    TVector<std::unique_ptr<IRule>> rewriteRightJoinsStageRules;
    rewriteRightJoinsStageRules.emplace_back(std::make_unique<TRewriteRightJoinRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Rewrite right joins", std::move(rewriteRightJoinsStageRules)));

    // Push predicates before inlining.
    TVector<std::unique_ptr<IRule>> earlyPushFilterRules;
    earlyPushFilterRules.emplace_back(std::make_unique<TExtractJoinExpressionsRule>());
    earlyPushFilterRules.emplace_back(std::make_unique<TExtractCommonConjunctsRule>());
    earlyPushFilterRules.emplace_back(std::make_unique<TPushFilterIntoJoinRule>());
    earlyPushFilterRules.emplace_back(std::make_unique<TPushFilterUnderMapRule>());
    earlyPushFilterRules.emplace_back(std::make_unique<TPushFilterThroughAggregateRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Push filters before inlining", std::move(earlyPushFilterRules)));

    // Subplan inlining. For correlated subqueries we create dependent join.
    TVector<std::unique_ptr<IRule>> inlineScalarSubPlanStageRules;
    inlineScalarSubPlanStageRules.emplace_back(std::make_unique<TInlineScalarSubplanRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Inline scalar subplans", std::move(inlineScalarSubPlanStageRules)));
    RBO.AddStage(std::make_unique<TConstantFoldingStage>());

    TVector<std::unique_ptr<IRule>> inlineSimpleSubPlanStageRules;
    inlineSimpleSubPlanStageRules.emplace_back(std::make_unique<TInlineSimpleInExistsSubplanRule>());
    inlineSimpleSubPlanStageRules.emplace_back(std::make_unique<TInlineGenericInExistsSubplanRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Inline in/exists subplans", std::move(inlineSimpleSubPlanStageRules)));

    // Decorrelation stage.
    TVector<std::unique_ptr<IRule>> decorrelationStageRules;
    // At first try to rewrite or completely eliminate a dependent join.
    decorrelationStageRules.emplace_back(std::make_unique<TRewriteDependentJoinToCrossJoinNoFreeVarsRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TRewriteDependentJoinToCrossJoinRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TEliminateDependentJoinDomainRule>());
    // Try to push dependent join.
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughFilterRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughMapRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughAggregateRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughUnionAllRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughJoinRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughReplicateRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughSortRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TPushDependentJoinThroughLimitRule>());
    decorrelationStageRules.emplace_back(std::make_unique<TDependentJoinNotSupportedRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Decorrelation", std::move(decorrelationStageRules)));

    // Rewrites can introduce new definitions. Remove dead ones and collapse
    // copies globally.
    RBO.AddStage(std::make_unique<TGlobalPruningStage>("Prune before inlining"));
    RBO.AddStage(std::make_unique<TGlobalInliningStage>("Inline definitions"));

    TVector<std::unique_ptr<IRule>> pushMapRules;
    pushMapRules.emplace_back(std::make_unique<TPushMapElementsThroughInputRule>());
    pushMapRules.emplace_back(std::make_unique<TPushMapElementsIntoMapRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Push map elements", std::move(pushMapRules)));

    // Logical state I
    TVector<std::unique_ptr<IRule>> logicalStage_I_Rules;
    logicalStage_I_Rules.emplace_back(std::make_unique<TMergeUnionAllRule>());
    logicalStage_I_Rules.emplace_back(std::make_unique<TExtractJoinExpressionsRule>());
    logicalStage_I_Rules.emplace_back(std::make_unique<TExtractCommonConjunctsRule>());
    logicalStage_I_Rules.emplace_back(std::make_unique<TPushFilterIntoJoinRule>());
    logicalStage_I_Rules.emplace_back(std::make_unique<TPushSimpleJoinFilterRule>());
    logicalStage_I_Rules.emplace_back(std::make_unique<TPushFilterUnderMapRule>());
    logicalStage_I_Rules.emplace_back(std::make_unique<TPushFilterThroughAggregateRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Logical rewrites I", std::move(logicalStage_I_Rules)));

    TVector<std::unique_ptr<IRule>> logicalStage_II_Rules;
    logicalStage_II_Rules.emplace_back(std::make_unique<TFuseFiltersRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TExtractJoinExpressionsRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TExtractCommonConjunctsRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TPushFilterIntoJoinRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TPushFilterUnderMapRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TPushFilterThroughAggregateRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TEliminateLeftJoinRule>());
    logicalStage_II_Rules.emplace_back(std::make_unique<TPushLimitIntoSortRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Logical rewrites II", std::move(logicalStage_II_Rules)));

    RBO.AddStage(std::make_unique<TGlobalPruningStage>("Pruning I", pruneKeyColumns));

    // Duplicate only after logical rewrites have removed unused work, while
    // reads and joins still have a logical representation supported by Copy().
    if (!allowChannelSpilling) {
        TVector<std::unique_ptr<IRule>> expandReplicateRules;
        expandReplicateRules.emplace_back(std::make_unique<TExpandReplicateRule>());
        RBO.AddStage(std::make_unique<TRuleBasedStage>("Expand shared subtrees", std::move(expandReplicateRules)));
        RBO.AddStage(std::make_unique<TGlobalInliningStage>("Inline duplicated definitions"));
    }

    // Physical stage.
    TVector<std::unique_ptr<IRule>> physicalStageRules;
    // For columnstore we push ranges before CBO.
    physicalStageRules.emplace_back(std::make_unique<TPushRangesRule>(NYql::EStorageType::ColumnStorage));
    physicalStageRules.emplace_back(std::make_unique<TPushOlapFilterRule>());
    physicalStageRules.emplace_back(std::make_unique<TPushOlapProjectionRule>());
    physicalStageRules.emplace_back(std::make_unique<TDisableBlocksOnColumnsLimitRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Physical rewrites I", std::move(physicalStageRules)));

    TVector<std::unique_ptr<IRule>> indexSelectionRules;
    indexSelectionRules.emplace_back(std::make_unique<TPushRangesRule>(NYql::EStorageType::RowStorage));
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Index selection rules", std::move(indexSelectionRules)));

    // CBO stages.
    TVector<std::unique_ptr<IRule>> initialCBOStageRules;
    initialCBOStageRules.emplace_back(std::make_unique<TPullUpMapOverCBORule>());
    initialCBOStageRules.emplace_back(std::make_unique<TBuildInitialCBOTreeRule>());
    initialCBOStageRules.emplace_back(std::make_unique<TExpandCBOTreeRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Prepare for CBO", std::move(initialCBOStageRules)));

    TVector<std::unique_ptr<IRule>> cboStageRules;
    cboStageRules.emplace_back(std::make_unique<TOptimizeCBOTreeRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Invoke CBO", std::move(cboStageRules)));

    TVector<std::unique_ptr<IRule>> cleanUpCBOStageRules;
    cleanUpCBOStageRules.emplace_back(std::make_unique<TInlineCBOTreeRule>());
    cleanUpCBOStageRules.emplace_back(std::make_unique<TPushFilterIntoJoinRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Clean up after CBO", std::move(cleanUpCBOStageRules)));
    // Only dead Map elements are pruned here, key columns included.
    RBO.AddStage(std::make_unique<TGlobalPruningStage>("Prune after CBO", /*pruneKeyColumns=*/true,
        EPruningScope::MapDefinitions));
    RBO.AddStage(std::make_unique<TGlobalInliningStage>("Inline after CBO"));


    TVector<std::unique_ptr<IRule>> inlineJoinFiltersAfterCBORules;
    inlineJoinFiltersAfterCBORules.emplace_back(std::make_unique<TInlineJoinFiltersRule>());
    inlineJoinFiltersAfterCBORules.emplace_back(std::make_unique<TFuseFiltersRule>());
    inlineJoinFiltersAfterCBORules.emplace_back(std::make_unique<TPushFilterIntoJoinRule>());
    inlineJoinFiltersAfterCBORules.emplace_back(std::make_unique<TPushSimpleJoinFilterRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Inline join filters after CBO", std::move(inlineJoinFiltersAfterCBORules)));

    RBO.AddStage(std::make_unique<TGlobalPruningStage>("Pruning II"));
    RBO.AddStage(std::make_unique<TGlobalInliningStage>("Inline after join filters"));

    // Index lookup join has a different representation from regular join, so we need a special rewrite rule.
    TVector<std::unique_ptr<IRule>> physicalJoinRules;
    physicalJoinRules.emplace_back(std::make_unique<TRewriteJoinToIndexLookupJoinRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Physical rewrites II", std::move(physicalJoinRules)));

    // Assign physical stages.
    RBO.AddStage(std::make_unique<TAssignStagesStage>());

    // Optimize physical stages.
    TVector<std::unique_ptr<IRule>> optimizePhysicalStagesRules;
    optimizePhysicalStagesRules.emplace_back(std::make_unique<TPropagateAggregateThroughStageRule>());
    optimizePhysicalStagesRules.emplace_back(std::make_unique<TPropagateTopSortThroughStageRule>());
    optimizePhysicalStagesRules.emplace_back(std::make_unique<TPropagateLimitThroughStageRule>());
    RBO.AddStage(std::make_unique<TRuleBasedStage>("Optimize physical stages", std::move(optimizePhysicalStagesRules)));

    RBO.AddStage(std::make_unique<TPropagateHashFuncStage>());
}

void TKqpRBOCleanupTransformer::Rewind() {
}



TAutoPtr<IGraphTransformer> CreateKqpNewRBOTransformer(TIntrusivePtr<TKqpOptimizeContext>& kqpCtx, TTypeAnnotationContext& typeCtx,
                                                       TAutoPtr<IGraphTransformer>&& backendTypeAnnTransformer, TKikimrTablesData& tables,
                                                       const TString& cluster, const TString& database, TActorSystem* actorSystem,
                                                       const NMiniKQL::IFunctionRegistry& funcRegistry, TIntrusivePtr<TKqlTransformContext> transformCtx) {
    return new TKqpNewRBOTransformer(kqpCtx, typeCtx, std::move(backendTypeAnnTransformer), tables, cluster, database,
                                     actorSystem, funcRegistry, transformCtx);
}

TAutoPtr<IGraphTransformer> CreateKqpRBOCleanupTransformer(TTypeAnnotationContext &typeCtx) {
    return new TKqpRBOCleanupTransformer(typeCtx);
}

} // namespace NKikimr::NKqp
