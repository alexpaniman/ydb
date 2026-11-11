#pragma once

#include "frontend/frontend.h"
#include "kqp_operator.h"
#include <ydb/core/kqp/common/kqp_query_metadata.h>

namespace NKikimr::NKqp {

struct TCompileTrace;

struct TNativeUniqueConstraint {
    TRboTable Table;
    TVector<TString> Keys;
};

struct TNativeIndex {
    TRboTable Table;
    TVector<TString> MainKeys;
};

struct TNativeTable {
    NFrontend::TCatalogSnapshot Schema;
    TRboTable Table;
    NYql::EStorageType Storage = NYql::EStorageType::RowStorage;
    TVector<TNativeUniqueConstraint> UniqueConstraints = {};
    // Resolve only explicitly requested views from the loaded catalog snapshot.
    std::function<TNativeIndex(std::string_view, NFrontend::TSourcePosition)> ResolveIndex = {};
};

using TNativeTableResolver = std::function<TNativeTable(const NFrontend::TTableAddress&, NFrontend::TSourcePosition)>;

// The checked compiler's complete handoff. Roots own scalar arenas and ordered
// result columns; parameters and execution metadata require no YQL graph.
struct TNativeQuery {
    std::shared_ptr<TCompileTrace> CompileTrace;
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
