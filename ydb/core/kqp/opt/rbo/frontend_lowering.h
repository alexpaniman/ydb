#pragma once

#include "frontend/expression.h"
#include "kqp_native_olap.h"

#include <yql/essentials/ast/yql_expr.h>

namespace NKikimr::NKqp {

// KQP supplies zero-dependency randomness once per query execution. These names
// are reserved by the backend and cannot be native SQL parameter identifiers.
inline constexpr TStringBuf QueryRandomUuidParameter = "%kqp%random_uuid";
inline constexpr TStringBuf QueryRandomNumberParameter = "%kqp%random_number";

class TOlapProgram;
class TExpression;
struct TRboTable;
class IOperator;
NYql::TExprNode::TPtr LowerTable(const TRboTable& table, NYql::TExprContext& ctx, NYql::TPositionHandle pos);
const NYql::TTypeAnnotationNode* ExportOperatorType(const IOperator& op, NYql::TExprContext& ctx);
const NYql::TTypeAnnotationNode* ExportIUType(const IOperator& op, unsigned int id, NYql::TExprContext& ctx);
namespace NFrontend { class TParameterType; struct TRowExpression; }

// The execution compatibility boundary. The native frontend and its type
// checker never include this header or depend on YQL. Each native operation
// has a finite, explicit lowering; no SQL/YQL normalization is performed here.
const NYql::TTypeAnnotationNode* ExportNativeType(NFrontend::TType type, NYql::TExprContext& ctx,
    const NFrontend::TExpressionArena* arena = nullptr);
NFrontend::TType ImportNativeType(const NYql::TTypeAnnotationNode& type, NFrontend::TExpressionArena* arena = nullptr);
const NYql::TTypeAnnotationNode* ExportNativeParameterType(const NFrontend::TParameterType& type, NYql::TExprContext& ctx);
NYql::TExprNode::TPtr LowerNativeParameter(const TString& name, const NFrontend::TParameterType& type,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos);
NYql::TExprNode::TPtr LowerNativeLambda(const NFrontend::TExpressionArena& arena,
    NFrontend::TExprId root, NYql::TExprContext& ctx, NYql::TPositionHandle pos);
NYql::TExprNode::TPtr LowerRowExpressionLambda(const NFrontend::TRowExpression& expression,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos);

// Physical builders explicitly cross the scalar execution boundary here.
// Native lowering produces a fresh backend graph owned by ctx; neither that
// graph nor its mutable backend annotations are retained in TExpression.
NYql::TExprNode::TPtr LowerExpressionLambda(const TExpression& expression, NYql::TExprContext& ctx);

NYql::TExprNode::TPtr LowerNativeOlapPredicate(const NFrontend::TExpressionArena& arena,
    NFrontend::TExprId root, NYql::TExprContext& ctx, NYql::TPositionHandle pos);
NYql::TExprNode::TPtr LowerNativeOlapProgram(const TOlapProgram& program,
    NYql::TExprContext& ctx, NYql::TPositionHandle pos);

} // namespace NKikimr::NKqp
