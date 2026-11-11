#include "bindings.h"

#include <unordered_set>

namespace NKikimr::NKqp::NFrontend {

TScalarBinding::TScalarBinding(std::shared_ptr<const TExpressionArena> arena, TExprId root,
    std::span<const TColumnId> parameters)
    : Arena_(std::move(arena)), Root_(root) {
    if (!Arena_ || root >= Arena_->Size()) throw TFrontendError({}, "external scalar binding requires a valid native body");
    for (std::size_t i = 0; i < parameters.size(); ++i)
        if (!Parameters_.emplace(parameters[i], i).second) throw TFrontendError({}, "duplicate external callable parameter");
    std::unordered_set<TExprId> visited;
    std::vector<TExprId> pending{root};
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!visited.insert(id).second) continue;
        const auto& node = (*Arena_)[id];
        if (node.Op == EOpcode::Parameter || (node.Op == EOpcode::Column && !Parameters_.contains(node.ColumnId)))
            throw TFrontendError(node.Position, "external scalar binding has an undeclared input");
        pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
    }
}

TExprId TScalarBinding::Apply(TExpressionArena& destination, std::span<const TExprId> arguments,
    TSourcePosition position) const {
    if (arguments.size() != Arity()) throw TFrontendError(position, "external callable argument count mismatch");
    try {
        return destination.Import(*Arena_, Root_, [&](TColumnId column, TType, TSourcePosition) {
            return arguments[Parameters_.at(column)];
        });
    } catch (const TFrontendError& error) {
        throw TFrontendError(position, error.what());
    }
}

} // namespace NKikimr::NKqp::NFrontend
