#pragma once

#include "kqp_info_unit.h"
#include "kqp_plan_props.h"
#include "frontend/expression.h"

#include <functional>
#include <memory>
#include <optional>


namespace NKikimr {
namespace NKqp {

using namespace NYql;

/**
 * Scalar expression handle for the query-owned native typed DAG.
 * Native handles share a query-owned append-only arena; existing roots remain
 * immutable when optimizer composition appends new nodes.
 */
class TExpression {
  public:
    static TExpression FromNative(std::shared_ptr<NFrontend::TExpressionArena> arena,
        NFrontend::TExprId root, TPlanProps* props = nullptr, TPositionHandle pos = {});

    bool IsNative() const { return bool(NativeArena_); }
    const std::shared_ptr<NFrontend::TExpressionArena>& GetNativeArena() const { return NativeArena_; }
    NFrontend::TExprId GetNativeRoot() const { return NativeRoot_; }
    NFrontend::TType GetNativeType() const { return (*NativeArena_)[NativeRoot_].Type; }
    TExpression WithNativeColumnTypes(const std::function<NFrontend::TType(TInfoUnitId)>& typeOf) const;
    TPositionHandle Pos() const { return NativePos_; }

    TExpression() = default;
    TExpression(const TExpression&) = default;
    TExpression(TExpression&&) noexcept = default;
    TExpression& operator=(const TExpression&) = default;
    TExpression& operator=(TExpression&&) noexcept = default;
    ~TExpression() = default;

    // Split a conjunct into a vector of expressions. If the is no conjunction at the top level,
    // just return a vector with this node.
    TVector<TExpression> SplitConjunct() const;

    // Split a disjunct into a vector of expressions. If the is no disjunction at the top level,
    // just return a vector with this node.
    TVector<TExpression> SplitDisjunct() const;

    // Check if the expression is just getting a single column from a tuple
    bool IsColumnAccess() const;
    // A direct column, optionally wrapped in a lossless nullable lift. This
    // inspects the scalar representation without generating backend nodes.
    std::optional<TInfoUnitId> GetColumnId(bool allowOptionalLift = false) const;
    std::optional<ui64> GetUint64Literal() const;

    // Check if the expression is a just a single callable on top of a column expression
    bool IsSingleCallable(const THashSet<TString>& allowedCallables) const;

    // Check if this is a potential equi-join condition
    bool MaybeEquiJoinCondition() const;

    // Check if this is a potential equi-join condition over simple expressions
    bool MaybeExprEquiJoinCondition() const;

    // Check if this is a potential comparison of a column with a constant
    bool MaybeConstantCondition() const;

    // Return all column references used in this expression
    // Optionally include columns that bind to subplan results and external columns inside correlated subqueries
    // Nonempty expressions need plan properties for subplan classification.
    // A subsequent GetInputIUs() call on the same
    // TExpression may refresh the returned buffer; do not retain references,
    // pointers, or iterators into it across calls.
    const TUnorderedIUs& GetInputIUs(bool includeSubplanVars = false, bool includeCorrelatedDeps = false) const Y_LIFETIME_BOUND;

    // Return direct row-reference IDs without subplan classification.
    // The result depends only on the scalar and is cached after its first traversal.
    const TUnorderedIUs& GetRawInputIUs() const Y_LIFETIME_BOUND;

    void BindPlanProps(TPlanProps* props) const {
        PlanProps = props;
    }

    // Simultaneous ID substitution, not a change to registry display labels.
    TExpression ApplyRenames(const TSubstitutions& substitutions) const;

    // Extract common conjuncts from OR branches.
    std::optional<TExpression> TryExtractCommonConjuncts() const;

    // Produce a pretty string for this expression
    TString ToString() const;

    // Produce a compact string suitable for explain output. Complex expressions are summarized by dependencies.
    TString ToExplainString(const TInfoUnitRegistry& registry) const;

    mutable TPlanProps* PlanProps = nullptr;

  private:
    std::shared_ptr<NFrontend::TExpressionArena> NativeArena_;
    NFrontend::TExprId NativeRoot_ = 0;
    TPositionHandle NativePos_;
    mutable bool NativeIUsComputed_ = false;

    bool MaybeEquiJoinConditionInternal(bool includeExpressions) const;

    mutable TUnorderedIUs RawInputIUs;
    // Reusable scratch buffer. Every resolving call refreshes it against the
    // current subplan registry, so registry mutations cannot stale the result.
    mutable TUnorderedIUs ResolvedInputIUs;

};

/**
 * Model a generic potential join condition
 */
class TEquiJoinCondition {
  public:

    TEquiJoinCondition(const TExpression& expr);

    // In case this is a simple predicate that contains a single column reference on each side, return left column
    TInfoUnitId GetLeftIU() const;

    // In case this is a simple predicate that contains a single column reference on each side, return right column
    TInfoUnitId GetRightIU() const;

    // Materialize computed equality operands as typed definitions and return
    // the equality over their column bindings. Native rewrites stay in the DAG.
    std::optional<TExpression> ExtractExpressions(TMappedIUs<TExpression>& expressions);

    const TExpression& Expr;
    TUnorderedIUs LeftIUs;
    TUnorderedIUs RightIUs;

    bool IncludesExpressions = true;
};

// Typed factories append directly to the query arena.
TExpression MakeColumnAccess(TInfoUnitId column, NFrontend::TType type,
    TPositionHandle pos, TPlanProps* props);
// Follow freshly constructed forwarding operators to their typed producer.
// Native factories need this before the next whole-plan property refresh.
TExpression MakeColumnAccess(TInfoUnitId column, IOperator& input,
    TPositionHandle pos, TPlanProps* props);

// Create a constant expression; native plans supply their destination arena through properties.
TExpression MakeConstant(const TString& type, const TString& value, TPositionHandle pos, TPlanProps* props = nullptr);

// Make a conjunction in the first operand's arena, preserving plan properties.
TExpression MakeConjunction(const TVector<TExpression>& vec, bool pgSyntax = false);

// Negate a predicate
TExpression MakeNegation(const TExpression& expr);

// Make a binary predicate with an arbitrary callable, extract context and properties from one of the arguments
TExpression MakeBinaryPredicate(const TString& callable, const TExpression& left, const TExpression& right);

// Make an unary callable
TExpression MakeUnaryCallable(const TString& callable, const TExpression& arg);

// Make ensure.
TExpression MakeEnsure(const TExpression& value, const TExpression& predicate, const TString& message);

}

}
