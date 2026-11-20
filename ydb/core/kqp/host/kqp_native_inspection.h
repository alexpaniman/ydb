#pragma once

#include "kqp_host.h"

#include <functional>

namespace NYql {
class TKikimrSessionContext;
}

namespace NKikimr::NKqp {

struct TNativeQuery;

using TNativeInspectionInput = std::function<TNativeQuery(NYql::TExprContext&, NYql::TKikimrSessionContext&)>;

IKqpHost::IAsyncQueryResultPtr PrepareNativeForInspection(IKqpHost& host,
    const std::shared_ptr<TCompileTrace>& trace, const TNativeInspectionInput& import);

} // namespace NKikimr::NKqp
