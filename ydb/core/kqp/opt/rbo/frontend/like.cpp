#include "like.h"

#include <util/charset/utf8.h>

#include <algorithm>
#include <limits>

namespace NKikimr::NKqp::NFrontend::NDetail {

TExprId BuildLike(TExpressionArena& arena, TExprId input, TExprId patternId,
    std::optional<TExprId> escapeId, TSourcePosition pos, bool caseSensitive) {
    std::string escape;
    if (escapeId) {
        const auto& node = arena[*escapeId];
        if (node.Op != EOpcode::Literal || node.Type != TType{ETypeKind::String} ||
            !std::holds_alternative<std::string>(node.Value))
            throw TFrontendError(pos, "ESCAPE requires a String literal");
        escape = std::get<std::string>(node.Value);
        if (escape.size() != 1 || static_cast<unsigned char>(escape[0]) >= 128 ||
            escape[0] == '%' || escape[0] == '_' || escape[0] == '\\')
            throw TFrontendError(pos, "ESCAPE requires one ASCII character other than %, _ or backslash");
    }
    const auto pattern = arena[patternId];
    if (pattern.Op != EOpcode::Literal || !pattern.Type.IsString() ||
        !std::holds_alternative<std::string>(pattern.Value))
        return arena.Like(input, patternId, std::move(escape), pos, caseSensitive);
    const auto& text = std::get<std::string>(pattern.Value);
    if (text.size() > std::numeric_limits<std::uint32_t>::max())
        throw TResourceLimitError(pos, "LIKE pattern exceeds Uint32 string length");
    struct TComponent {
        bool Simple = true;
        std::string Prefix;
        std::string Suffix;
    };
    std::vector<TComponent> components(1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (!escape.empty() && ch == escape[0]) {
            if (++i == text.size()) throw TFrontendError(pos, "LIKE pattern must not end with an escape character");
            ch = text[i];
        } else if (ch == '%') {
            components.emplace_back();
            continue;
        } else if (ch == '_') {
            components.back().Simple = false;
            components.back().Suffix.clear();
            continue;
        }
        auto& part = components.back();
        if (part.Simple) part.Prefix += ch;
        part.Suffix += ch;
    }
    if (!caseSensitive) {
        // Literal patterns without cased characters use the ordinary LIKE
        // simplifications, including their nullable result contracts. All
        // other ILIKE patterns retain the runtime's Unicode case folding.
        const bool invariant = pattern.Type.Kind == ETypeKind::Utf8
            ? ToLowerUTF8(text) == ToUpperUTF8(text)
            : std::none_of(text.begin(), text.end(), [](unsigned char ch) {
                return ch >= 128 || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            });
        if (!invariant) return arena.Like(input, patternId, std::move(escape), pos, false);
    }
    if (std::any_of(components.begin(), components.end(), [](const auto& part) { return !part.Simple; })) {
        auto result = arena.Like(input, patternId, std::move(escape), pos);
        // SQL retains its nullable prefix/suffix predicate around a regex.
        const auto& prefix = components.front().Prefix;
        const auto& suffix = components.back().Suffix;
        if (!prefix.empty() || !suffix.empty()) {
            const auto literal = arena.Literal(pattern.Type.Required(), prefix.empty() ? suffix : prefix, pos);
            const auto test = arena.Binary(prefix.empty() ? EOpcode::EndsWith : EOpcode::StartsWith, input, literal, pos);
            result = arena.Binary(EOpcode::And, test, result, pos);
        }
        return result;
    }
    const bool leading = components.size() > 1 && components.front().Prefix.empty();
    const bool trailing = components.size() > 1 && components.back().Prefix.empty();
    std::vector<std::string> pieces;
    for (auto& part : components) if (!part.Prefix.empty()) pieces.push_back(std::move(part.Prefix));
    if (pieces.size() <= 1) {
        const auto literal = arena.Literal(pattern.Type.Required(), pieces.empty() ? std::string{} : pieces.front(), pos);
        const auto op = leading && trailing ? EOpcode::Contains : leading ? EOpcode::EndsWith : trailing ? EOpcode::StartsWith : EOpcode::Equal;
        return arena.Binary(op, input, literal, pos);
    }
    // YQL uses regex matching for these forms: NULL/invalid UTF-8 returns
    // false. A leading literal also adds a nullable prefix predicate.
    // Preserve both its values and its metadata without retaining regexes.
    const bool regexSemantics = pieces.size() > 2 || leading || trailing;
    if (arena[input].Type.Kind == ETypeKind::Null)
        return arena.Literal({ETypeKind::Bool, !regexSemantics || !leading}, regexSemantics ? TScalar{false} : TScalar{std::monostate{}}, pos);
    if (!arena[input].Type.IsString()) throw TFrontendError(pos, "LIKE requires a string input");
    const bool nullable = arena[input].Type.OptionalDepth;
    auto result = regexSemantics ? arena.Unary(EOpcode::IsNotNull, arena.Cast(input, {ETypeKind::Utf8}, pos), pos) :
        arena.Literal({ETypeKind::Bool}, true, pos);
    // % consumes any byte sequence. Ordered searches need no regex object,
    // substring copies or semantic lowering in the execution model.
    input = arena.Cast(input, {ETypeKind::String}, pos);
    const auto zero = arena.Literal({ETypeKind::Uint32}, std::uint64_t{0}, pos);
    auto offset = zero;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const auto literal = arena.Literal({ETypeKind::String}, pieces[i], pos);
        const auto length = arena.Literal({ETypeKind::Uint32}, static_cast<std::uint64_t>(pieces[i].size()), pos);
        TExprId matches;
        if (i == 0 && !leading) {
            matches = arena.Binary(EOpcode::StartsWith, input, literal, pos);
            offset = length;
        } else if (i + 1 == pieces.size() && !trailing) {
            // A suffix may occur earlier too; require the final occurrence
            // to start after the already consumed prefix (no overlap).
            const auto end = arena.Binary(EOpcode::Add, arena.Convert(offset, {ETypeKind::Uint64}, pos), arena.Convert(length, {ETypeKind::Uint64}, pos), pos);
            const auto fits = arena.Binary(EOpcode::GreaterEqual, arena.Unary(EOpcode::Length, input, pos), end, pos);
            matches = arena.Binary(EOpcode::And, arena.Binary(EOpcode::EndsWith, input, literal, pos), fits, pos);
        } else {
            const auto found = arena.Find(input, literal, offset, pos);
            matches = arena.Unary(EOpcode::IsNotNull, found, pos);
            const TExprId alternatives[]{found, zero};
            offset = arena.Binary(EOpcode::Add, arena.Coalesce(alternatives, pos), length, pos);
        }
        result = arena.Binary(EOpcode::And, result, matches, pos);
    }
    if (regexSemantics) {
        const TExprId alternatives[]{result, arena.Literal({ETypeKind::Bool}, false, pos)};
        result = arena.Coalesce(alternatives, pos);
        if (!leading && nullable) result = arena.Convert(result, {ETypeKind::Bool, true}, pos);
    } else if (nullable)
        result = arena.If(arena.Unary(EOpcode::IsNull, input, pos), arena.Literal({ETypeKind::Bool, true}, std::monostate{}, pos), result, pos);
    return result;
}


} // namespace NKikimr::NKqp::NFrontend::NDetail
