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
    // Keep complex matching atomic through RBO. Expanding it into searches
    // changes the predicates seen by storage admission and selectivity. Its
    // runtime matcher is constructed only at the terminal emission boundary.
    auto result = arena.Like(input, patternId, std::move(escape), pos);
    if (!leading) {
        const auto prefix = arena.Literal(pattern.Type.Required(), pieces.front(), pos);
        const auto starts = arena.Binary(EOpcode::StartsWith, input, prefix, pos);
        if (pieces.size() == 2 && !trailing) {
            // The established prefix%suffix simplification also checks length
            // so the prefix and suffix cannot overlap.
            const auto length = arena.Literal({ETypeKind::Uint32},
                static_cast<std::uint64_t>(pieces.front().size() + pieces.back().size()), pos);
            const auto fits = arena.Binary(EOpcode::GreaterEqual, arena.Unary(EOpcode::Length, input, pos), length, pos);
            const auto suffix = arena.Literal(pattern.Type.Required(), pieces.back(), pos);
            result = arena.Binary(EOpcode::And, fits, arena.Binary(EOpcode::And, starts,
                arena.Binary(EOpcode::EndsWith, input, suffix, pos), pos), pos);
        } else {
            result = arena.Binary(EOpcode::And, starts, result, pos);
        }
    }
    return result;
}


} // namespace NKikimr::NKqp::NFrontend::NDetail
