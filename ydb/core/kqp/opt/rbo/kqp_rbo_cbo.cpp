#include "kqp_rbo_cbo.h"
#include "kqp_rbo_lookup_join.h"
#include "kqp_rbo_utils.h"

namespace {

using namespace NYql;
using namespace NKikimr::NKqp;

bool IsSharedRelNode(const std::shared_ptr<IBaseOptimizerNode>& node) {
    return node->Kind == EOptimizerNodeKind::RelNodeType
        && std::static_pointer_cast<TRBORelOptimizerNode>(node)->Op->Kind == EOperator::Replicate;
}

bool IsLookupJoinApplicable(const std::shared_ptr<IBaseOptimizerNode>& left,
    const std::shared_ptr<IBaseOptimizerNode>& right,
    const TVector<TJoinColumn>& leftJoinKeys, const TVector<TJoinColumn>& rightJoinKeys,
    EJoinKind joinKind, const TRBOProviderContext& provider) {
    if (right->Kind != EOptimizerNodeKind::RelNodeType || leftJoinKeys.size() != rightJoinKeys.size()
        || right->Stats.StorageType != EStorageType::RowStorage || right->Stats.Type != EStatisticsType::BaseTable) {
        return false;
    }
    TVector<TTypedLookupJoinKey> keys;
    for (size_t i = 0; i < leftJoinKeys.size(); ++i) {
        const auto leftId = GetCBOColumnId(leftJoinKeys[i]);
        const auto* type = provider.BoundaryTypes.Find(leftId);
        if (!type) {
            return false;
        }
        keys.push_back({leftId, GetCBOColumnId(rightJoinKeys[i]), *type,
            leftJoinKeys[i].EqualNulls || rightJoinKeys[i].EqualNulls});
    }
    const auto& rightOp = std::static_pointer_cast<TRBORelOptimizerNode>(right)->Op;
    return PrepareLookupJoin(rightOp, keys, ConvertToJoinString(joinKind), IsSharedRelNode(left), false,
        provider.KqpCtx, provider.Registry, provider.ExprCtx).has_value();
}

} // anonymous namespace

namespace NKikimr::NKqp::NOpt {

bool TRBOProviderContext::IsJoinApplicable(const std::shared_ptr<IBaseOptimizerNode>& left,
    const std::shared_ptr<IBaseOptimizerNode>& right,
    const TVector<TJoinColumn>& leftJoinKeys,
    const TVector<TJoinColumn>& rightJoinKeys,
    EJoinAlgoType joinAlgo,
    EJoinKind joinKind) {

    switch( joinAlgo ) {
        case EJoinAlgoType::LookupJoin: {
            if ((OptLevel != 3) && (left->Stats.Nrows > 5000)) {
                return false;
            }
            return IsLookupJoinApplicable(left, right, leftJoinKeys, rightJoinKeys, joinKind, *this);
        }
        // FIXME: Don't pick reverse lookup join yet
        /*
        case EJoinAlgoType::LookupJoinReverse: {
            if (joinKind != EJoinKind::LeftSemi) {
                return false;
            }
            if ((OptLevel != 3) && (right->Stats.Nrows > 5000)) {
                return false;
            }
            return IsLookupJoinApplicable(right, left, rightJoinKeys, leftJoinKeys, joinKind, *this);
        }
        */
        case EJoinAlgoType::MapJoin:
            return joinKind != EJoinKind::OuterJoin && joinKind != EJoinKind::Exclusion && right->Stats.ByteSize < 1e6;
        case EJoinAlgoType::GraceJoin:
            return true;
        case EJoinAlgoType::ReverseBlockJoin:
            return BlockJoinEnabled && (joinKind == EJoinKind::LeftJoin | joinKind == EJoinKind::LeftOnly | joinKind == EJoinKind::LeftSemi);
        default:
            return false;
    }
}
}
