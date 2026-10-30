#pragma once

#include "frontend/frontend.h"
#include "kqp_operator.h"

namespace NKikimr::NKqp {

struct TNativeTable {
    NFrontend::TCatalogTable Schema;
    NYql::TExprNode::TPtr Table;
    NYql::EStorageType Storage = NYql::EStorageType::RowStorage;
};

using TNativeTableResolver = std::function<TNativeTable(std::string_view, NFrontend::TSourcePosition)>;

// Consumes an already bound program after asynchronous native catalog binding.
// This is the boundary at which existing RBO table/type wrappers may be used.
TVector<TIntrusivePtr<TOpRoot>> ConvertNativeToRbo(NFrontend::TCompiledProgram program,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx);

// Tables must be resolved through the caller's normal catalog/authorization
// path. The returned roots own native scalars; no YQL parser, CompileExpr,
// RewriteSelect, PlanConverter, or YQL type transformer is invoked.
TVector<TIntrusivePtr<TOpRoot>> CompileNativeToRbo(std::string_view sql,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx,
    const NFrontend::TBindSettings& bindSettings = {}, const NFrontend::TParseSettings& parseSettings = {});

} // namespace NKikimr::NKqp
