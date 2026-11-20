#pragma once

#include <ydb/core/kqp/opt/rbo/kqp_operator.h>

namespace NKikimr::NKqp::NParsedQueryBench {

TString BuildLogicalSignature(const TVector<TIntrusivePtr<TOpRoot>>& roots, NYql::TExprContext& ctx);

} // namespace NKikimr::NKqp::NParsedQueryBench
