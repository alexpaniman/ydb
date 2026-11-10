#pragma once

#include "frontend/frontend.h"
#include "kqp_operator.h"
#include <ydb/core/kqp/common/kqp_query_metadata.h>

namespace NKikimr::NKqp {

struct TNativeTable {
    NFrontend::TCatalogTable Schema;
    TRboTable Table;
    NYql::EStorageType Storage = NYql::EStorageType::RowStorage;
};

using TNativeTableResolver = std::function<TNativeTable(std::string_view, NFrontend::TSourcePosition)>;

// The checked compiler's complete handoff. Roots own scalar arenas and ordered
// result columns; parameters and execution metadata require no YQL graph.
struct TNativeQuery {
    TVector<TIntrusivePtr<TOpRoot>> Roots;
    std::unordered_map<std::string, NFrontend::TParameterType> Parameters;
    TQueryExecutionMetadata Execution;
};

// Consumes an already bound program after asynchronous native catalog binding.
// Table identities and schemas remain values throughout native optimization.
TNativeQuery ConvertNativeToRbo(NFrontend::TCompiledProgram program,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx, NYql::TKikimrConfiguration* config = nullptr);

// Tables must be resolved through the caller's normal catalog/authorization
// path. The returned roots own native scalars; no YQL parser, CompileExpr,
// RewriteSelect, PlanConverter, or YQL type transformer is invoked.
TNativeQuery CompileNativeToRbo(std::string_view sql,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx,
    const NFrontend::TBindSettings& bindSettings = {}, const NFrontend::TParseSettings& parseSettings = {},
    NYql::TKikimrConfiguration* config = nullptr);

} // namespace NKikimr::NKqp
