#pragma once

#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/tests/tools/parsed_query_bench/proto/logical_plan.pb.h>

namespace NKikimr::NKqp::NParsedQueryBench {

TNativeQuery ImportNativeLogicalPlan(const Ydb::Query::LogicalPlan& plan, const TNativeTableResolver& resolve,
    NYql::TExprContext& ctx, const NFrontend::TBindSettings& settings = {});

} // namespace NKikimr::NKqp::NParsedQueryBench
