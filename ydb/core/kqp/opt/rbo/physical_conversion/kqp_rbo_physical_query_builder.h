#pragma once

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/opt/rbo/kqp_stage_graph.h>

#include <util/generic/noncopyable.h>

namespace NKikimr::NKqp {

class TOpRoot;
class TOpTableEffect;
class TRBOContext;

class TPhysicalQueryBuilder : public NNonCopyable::TNonCopyable {
public:
    TPhysicalQueryBuilder(const TVector<TIntrusivePtr<TOpRoot>>& roots, TVector<TStageGraph>&& graph, TVector<THashMap<ui32, NYql::TExprNode::TPtr>>&& stages, TVector<THashMap<ui32, TVector<NYql::TExprNode::TPtr>>>&& stageArgs,
                          TVector<THashMap<ui32, NYql::TPositionHandle>>&& stagePos, TVector<TPhysicalNames>&& names, TRBOContext& rboCtx);

    NYql::TExprNode::TPtr BuildPhysicalQuery();
    TPhysicalQueryBuilder() = delete;
    ~TPhysicalQueryBuilder() = default;

private:
    TVector<NYql::TExprNode::TPtr> BuildPhysicalStageGraph(int rootIdx);
    TVector<NYql::TExprNode::TPtr> LowerPhysicalStageCompatibility(TVector<NYql::TExprNode::TPtr>&& physicalStages);
    TVector<NYql::TExprNode::TPtr> PreparePhysicalStages(TVector<NYql::TExprNode::TPtr>&& physicalStages);
    NYql::TExprNode::TPtr BuildPhysicalQuery(TVector<TVector<NYql::TExprNode::TPtr>>&& physicalStages);
    NYql::TExprNode::TPtr BuildDqPhyStage(const TVector<NYql::TExprNode::TPtr>& inputs, const TVector<NYql::TExprNode::TPtr>& args, NYql::TExprNode::TPtr physicalStageBody,
        NYql::NNodes::TCoNameValueTupleList&& setings, NYql::TExprContext& ctx, NYql::TPositionHandle pos) const;
    NYql::TExprNode::TPtr BuildDqPhySinkStage(const TVector<NYql::TExprNode::TPtr>& inputs, const TVector<NYql::TExprNode::TPtr>& args, NYql::TExprNode::TPtr physicalStageBody,
        NYql::NNodes::TCoNameValueTupleList&& setings, const NYql::TExprNode::TPtr& sinkSettings, const TOpTableEffect& effect, NYql::TExprContext& ctx, NYql::TPositionHandle pos);
    void TopologicalSort(NYql::NNodes::TDqPhyStage& dqStage, TVector<NYql::TExprNode::TPtr>& result, THashSet<const NYql::TExprNode*>& visited) const;
    void TopologicalSort(NYql::NNodes::TDqPhyStage&& dqStage, TVector<NYql::TExprNode::TPtr>& result) const;
    NYql::TExprNode::TPtr LowerExecutionCompatibility(NYql::TExprNode::TPtr root);
    void TypeAnnotate(NYql::TExprNode::TPtr& input);
    NYql::TKqpPhyQuerySettings GetPhysicalQuerySettings() const;
    NYql::TKqpPhyTxSettings GetPhysicalTxSettings(size_t rootIdx) const;
    NYql::TExprNode::TPtr GetFinalStage(const NYql::TExprNode::TPtr& stage) const;
    bool NeedFinalNarrowing(int rootIdx);
    NYql::TExprNode::TPtr BuildFinalNarrowStage(int rootIdx, const NYql::TExprNode::TPtr& stage) const;
    TVector<NYql::NNodes::TKqpParamBinding> CollectParamBindings(int rootIdx, const NYql::TExprNode::TPtr& rootStage);
    NYql::TExprNode::TPtr BuildMaterialize(int rootIdx, NYql::TExprNode::TPtr ranges);
    bool IsSingleTaskConnection(const NYql::NNodes::TExprBase& input) const;

    const TVector<TIntrusivePtr<TOpRoot>>& Roots;
    TVector<TStageGraph> Graphs;
    TVector<THashMap<ui32, NYql::TExprNode::TPtr>> Stages;
    TVector<THashMap<ui32, TVector<NYql::TExprNode::TPtr>>> StageArgs;
    TVector<THashMap<ui32, NYql::TPositionHandle>> StagePos;
    TVector<TPhysicalNames> Names;
    ui32 UniqueParamsId{0};
    // Param and PhysicalTx
    TVector<TVector<std::pair<NYql::TExprNode::TPtr, NYql::TExprNode::TPtr>>> Materialize;
    TRBOContext& RBOCtx;
    static constexpr TStringBuf ParamBindingName = "%kqp_physical_tx_param_binding_";
};

} // namespace NKikimr::NKqp
