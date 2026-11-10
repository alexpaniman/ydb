#pragma once

#include <ydb/core/protos/kqp_physical.pb.h>
#include <util/generic/vector.h>

namespace NKikimr::NKqp {

// Physical compilation consumes execution metadata, independently of the
// frontend's expression representation. The legacy entry extracts these values
// from query blocks; the native entry supplies them directly.
struct TQueryExecutionMetadata {
    TVector<NKqpProto::TKqpTableOp> TableOperations;
    TVector<bool> DiscardResults;
    bool HasUncommittedChangesRead = false;
};

} // namespace NKikimr::NKqp
