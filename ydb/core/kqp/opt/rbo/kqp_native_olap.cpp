#include "kqp_native_olap.h"

#include <optional>
#include <unordered_map>

namespace NKikimr::NKqp {
namespace NDetail {
using K = NFrontend::ETypeKind;
using O = NFrontend::EOpcode;

TStringBuf OlapBinaryName(O op) {
    switch (op) {
        case O::Equal: return "eq";
        case O::NotEqual: return "neq";
        case O::Less: return "lt";
        case O::LessEqual: return "lte";
        case O::Greater: return "gt";
        case O::GreaterEqual: return "gte";
        case O::Add: return "+";
        case O::Subtract: return "-";
        case O::Multiply: return "*";
        case O::Divide: return "/";
        case O::Modulo: return "%";
        case O::Coalesce: return "??";
        case O::Contains: return "string_contains";
        case O::StartsWith: return "starts_with";
        case O::EndsWith: return "ends_with";
        default: return {};
    }
}

bool OlapIdentityConversion(const NFrontend::TExpressionArena& arena, const NFrontend::TExprNode& node) {
    if (node.Op != O::Convert) return false;
    const auto from = arena[node.Arguments[0]].Type;
    return from.Required() == node.Type.Required() || (from.Kind == K::Utf8 && node.Type.Kind == K::String);
}

} // namespace NDetail

namespace {
using K = NFrontend::ETypeKind;
using O = NFrontend::EOpcode;
using NDetail::OlapBinaryName;
using NDetail::OlapIdentityConversion;

bool OlapNodeSupported(const NFrontend::TExpressionArena& arena, const NFrontend::TExprNode& node) {
    if (node.Type.IsCollection() || node.Type.OptionalDepth > 1 || node.Type.IsJson() || node.Type.Kind == K::Decimal || node.Type.Kind == K::Calendar || node.Type.Kind == K::Null) return false;
    if (OlapIdentityConversion(arena, node)) return true;
    if (node.Op >= O::Add && node.Op <= O::Modulo && !node.Type.IsNumeric()) {
        const bool temporal = (node.Op == O::Add || node.Op == O::Subtract) &&
            (node.Type.IsDate() || node.Type.IsInterval());
        if (!temporal) return false;
    }
    if (!OlapBinaryName(node.Op).empty()) {
        // Storage string kernels return a required Bool and map NULL to false.
        // Nullable native predicates need the scalar evaluator to preserve
        // their type and three-valued semantics under NOT, COALESCE, etc.
        if (node.Type.OptionalDepth &&
            (node.Op == O::Contains || node.Op == O::StartsWith || node.Op == O::EndsWith)) return false;
        if (node.Arguments.size() != 2) return false;
        // The existing storage kernels cannot consume the same column twice.
        auto column = [&](NFrontend::TExprId id) -> std::optional<NFrontend::TColumnId> {
            while (OlapIdentityConversion(arena, arena[id])) id = arena[id].Arguments[0];
            return arena[id].Op == O::Column ? std::optional{arena[id].ColumnId} : std::nullopt;
        };
        const auto left = column(node.Arguments[0]), right = column(node.Arguments[1]);
        return !left || !right || *left != *right;
    }
    switch (node.Op) {
        case O::Literal: return !std::holds_alternative<std::monostate>(node.Value);
        case O::Column: case O::Parameter: case O::And: case O::Or: case O::Not: case O::If:
            return true;
        case O::Negate: case O::Abs: return node.Type.IsNumeric();
        case O::IsNull: case O::IsNotNull: return arena[node.Arguments[0]].Op == O::Column;
        default: return false;
    }
}

bool OlapScalarNodeSupported(const NFrontend::TExprNode& node) {
    if (node.Type.IsCollection() || node.Type.OptionalDepth > 1 || node.Type.IsJson() ||
        node.Type.Kind == K::Decimal || node.Type.Kind == K::Calendar) return false;
    if (!OlapBinaryName(node.Op).empty()) return true;
    switch (node.Op) {
        case O::Literal: case O::Column: case O::Parameter: case O::Convert: case O::Cast:
        case O::Negate: case O::Not: case O::And: case O::Or: case O::If:
        case O::IsNull: case O::IsNotNull: case O::Abs: case O::Length: case O::OptionalWrap:
        case O::Find: case O::Substring: case O::Concat: case O::Like: case O::ILike:
        case O::Lower: case O::Upper:
            return true;
        default: return false;
    }
}

} // namespace

bool CanLowerNativeOlapPredicate(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root, bool allowScalarApply) {
    if (arena[root].Type.Kind != K::Bool) return false;
    const auto& predicate = arena[root];
    if (predicate.Op >= O::Equal && predicate.Op <= O::GreaterEqual) {
        // A comparison with a NULL constant cannot select a row; preserve the
        // existing policy of leaving it out of the storage filter program.
        for (const auto arg : predicate.Arguments)
            if (arena[arg].Op == O::Literal && std::holds_alternative<std::monostate>(arena[arg].Value)) return false;
    }
    std::unordered_map<NFrontend::TExprId, bool> hasColumn;
    std::vector<std::pair<NFrontend::TExprId, bool>> pending{{root, false}};
    while (!pending.empty()) {
        const auto [id, ready] = pending.back(); pending.pop_back();
        if (hasColumn.contains(id)) continue;
        const auto& node = arena[id];
        if (!OlapNodeSupported(arena, node) && !(allowScalarApply && OlapScalarNodeSupported(node))) return false;
        if (!ready) {
            pending.emplace_back(id, true);
            for (auto arg : node.Arguments) pending.emplace_back(arg, false);
            continue;
        }
        bool column = node.Op == O::Column;
        for (auto arg : node.Arguments) {
            // Storage streaming logic needs a row count before combining
            // branches. Keep scalar-only Boolean branches in compute.
            if (!allowScalarApply && (node.Op == O::And || node.Op == O::Or) && !hasColumn.at(arg)) return false;
            column |= hasColumn.at(arg);
        }
        hasColumn.emplace(id, column);
    }
    return hasColumn.at(root);
}

bool CanLowerNativeOlapProjection(const NFrontend::TExpressionArena& arena, NFrontend::TExprId root) {
    const auto& node = arena[root];
    if (node.Op != O::JsonValue || arena[node.Arguments[0]].Op != O::Column) return false;
    // The storage path accessor accepts members and array indexes, but not
    // wildcard selectors. Quoted asterisks are ordinary member names. Native
    // path validation already excludes escapes inside quoted member names.
    char quote = 0;
    for (const char c : node.Name) {
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '*') return false;
    }
    return true;
}

} // namespace NKikimr::NKqp
