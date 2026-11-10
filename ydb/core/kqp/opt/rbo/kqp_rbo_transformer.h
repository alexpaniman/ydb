#pragma once

#include "kqp_rbo.h"

#include <ydb/core/kqp/opt/cbo/solver/kqp_opt_predicate_selectivity.h>
#include <ydb/core/kqp/opt/kqp_column_statistics_utils.h>
#include <ydb/library/actors/core/actorsystem_fwd.h>

#include <yql/essentials/ast/yql_expr.h>
#include <yql/essentials/core/yql_graph_transformer.h>

namespace NYql {

struct TTypeAnnotationContext;
struct TExprContext;
class TKikimrTablesData;

} // namespace NYql

namespace NMiniKQL {

class IFunctionRegistry;

} // namespace NMiniKQL

namespace NKikimr::NKqp {

struct TKqlTransformContext;
class IOperator;
class TExpression;
class TOpRoot;
class TOpJoin;
struct TPhysicalOpProps;

namespace NOpt {

struct TKqpOptimizeContext;

} // namespace NOpt

class TKqpNewRBOTransformer : public NYql::TGraphTransformerBase {
public:
    TKqpNewRBOTransformer(TIntrusivePtr<NOpt::TKqpOptimizeContext>& kqpCtx, NYql::TTypeAnnotationContext& typeCtx, TAutoPtr<NYql::IGraphTransformer>&& backendTypeAnnTransformer,
                          NYql::TKikimrTablesData& tables, const TString& cluster, const TString& database,
                          NActors::TActorSystem* actorSystem, const NMiniKQL::IFunctionRegistry& funcRegistry, TIntrusivePtr<TKqlTransformContext> transformCtx);
    // Main method of the transformer
    NYql::IGraphTransformer::TStatus DoTransform(NYql::TExprNode::TPtr input, NYql::TExprNode::TPtr& output, NYql::TExprContext& ctx) final;
    NThreading::TFuture<void> DoGetAsyncFuture(const NYql::TExprNode& input) final;
    TStatus DoApplyAsyncChanges(NYql::TExprNode::TPtr input, NYql::TExprNode::TPtr& output, NYql::TExprContext& ctx) final;
    void AddPlans(std::optional<NJson::TJsonValue> execPlan, std::optional<NJson::TJsonValue> explainPlan);

    // Supply the checked native roots before starting optimization.
    void SetNativeRoots(TVector<TIntrusivePtr<TOpRoot>> roots);

    void Rewind() override;

private:
    TStatus RequestColumnStatistics(NYql::TExprContext& ctx);
    TStatus ContinueOptimizations(NYql::TExprNode::TPtr input, NYql::TExprNode::TPtr& output, NYql::TExprContext& ctx);
    bool IsSuitableToRequestStatistics();
    void CollectTablesAndColumnsNames(NYql::TExprContext& ctx);
    void CollectTablesAndColumnsNames(IOperator* op, const TColumnLineage& lineage);
    void CollectTablesAndColumnsNames(const TExpression& expr, const IOperator& input, const TColumnLineage& lineage);
    void CollectJoinKeysColumns(TOpJoin* join, const TColumnLineage& lineage);
    void CollectJoinKeysTuple(const TVector<const TColumnLineageEntry*>& joinKeys);
    std::optional<TVector<TString>> FindEqHeightHistogramTuple(const TString& tableName, const THashSet<TString>& columns) const;
    bool IsSuitableToCollectStatistics(IOperator* op) const;
    void ApplyColumnStatistics();
    void InitializeRBOOptimizationStages();

    NYql::TTypeAnnotationContext& TypeCtx;
    NOpt::TKqpOptimizeContext& KqpCtx;
    TAutoPtr<NYql::IGraphTransformer> BackendTypeAnnTransformer;
    const NMiniKQL::IFunctionRegistry& FuncRegistry;
    TIntrusivePtr<TKqlTransformContext> TransformCtx;

    // Explain plan holder
    std::optional<NJson::TJsonValue> PlanJson;

    // Special fields to request column statistics.
    NYql::TKikimrTablesData& Tables;
    TString Cluster;
    TString Database;
    NActors::TActorSystem* ActorSystem;
    std::shared_ptr<TColumnStatisticsSharedState> SharedState;
    NThreading::TFuture<void> ColumnStatisticsReadiness;
    THashMap<TString, THashSet<TString>> CMColumnsByTableName;
    THashMap<TString, THashSet<TString>> HistColumnsByTableName;
    THashMap<TString, THashMap<TString, TVector<TString>>> EqHeightHistTuplesByTableName;

    bool NativeOptimized = false;

    TVector<TIntrusivePtr<TOpRoot>> Roots;
    TRuleBasedOptimizer RBO;
};

TAutoPtr<NYql::IGraphTransformer> CreateKqpNewRBOTransformer(TIntrusivePtr<NOpt::TKqpOptimizeContext>& kqpCtx, NYql::TTypeAnnotationContext& typeCtx,
                                                       TAutoPtr<NYql::IGraphTransformer>&& backendTypeAnnTransformer, NYql::TKikimrTablesData& tables,
                                                       const TString& cluster, const TString& database, NActors::TActorSystem* actorSystem,
                                                       const NMiniKQL::IFunctionRegistry& funcRegistry, TIntrusivePtr<TKqlTransformContext> transformCtx);

class TKqpRBOCleanupTransformer : public NYql::TSyncTransformerBase {
public:
    TKqpRBOCleanupTransformer(NYql::TTypeAnnotationContext& typeCtx)
        : TypeCtx(typeCtx) {
    }

    // Main method of the transformer
    NYql::IGraphTransformer::TStatus DoTransform(NYql::TExprNode::TPtr input, NYql::TExprNode::TPtr& output, NYql::TExprContext& ctx) final;
    void Rewind() override;

private:
    NYql::TTypeAnnotationContext& TypeCtx;
};

TAutoPtr<NYql::IGraphTransformer> CreateKqpRBOCleanupTransformer(NYql::TTypeAnnotationContext& typeCtx);

} // namespace NKikimr::NKqp
