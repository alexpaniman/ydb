#pragma once
#include "kqp_rbo_physical_op_builder.h"
#include <yql/essentials/utils/log/log.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

class TPhysicalTableEffectBuilder: public TPhysicalUnaryOpBuilder {
public:
    TPhysicalTableEffectBuilder(TOpTableEffect& tableEffect, TExprContext& ctx, TPositionHandle pos, const TPhysicalNames& names)
        : TPhysicalUnaryOpBuilder(ctx, pos, names), TableEffect(tableEffect) {}

    TExprNode::TPtr BuildPhysicalOp(TExprNode::TPtr input) override;

private:
    TOpTableEffect& TableEffect;
};

namespace NKikimr::NKqp {
NYql::TExprNode::TPtr LowerTableSinkSettings(const TTableSinkSettings& sink, NYql::TExprContext& ctx, NYql::TPositionHandle pos, ui64 priority = 0);
// Input stage already exposes storage column names and has backend types.
// Runtime generation uses the same catalog defaults as ordinary table writes.
NYql::TExprNode::TPtr LowerTableDefaultConnection(NYql::TExprNode::TPtr inputStage,
    const TOpTableEffect& effect, NYql::TExprContext& ctx);
NYql::TExprNode::TPtr LowerTableGeneratedColumns(NYql::TExprNode::TPtr input,
    const TOpTableEffect& effect, NYql::TExprContext& ctx);
}
