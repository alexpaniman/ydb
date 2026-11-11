#pragma once

#include "expression.h"

namespace NKikimr::NKqp::NFrontend {

// A native scalar body with explicit formal columns. Construction rejects free
// columns and runtime parameters; call arguments supply all external inputs.
// The source arena is shared immutably, and each use imports only its body DAG.
class TScalarBinding {
public:
    TScalarBinding(std::shared_ptr<const TExpressionArena> arena, TExprId root,
        std::span<const TColumnId> parameters = {});
    std::size_t Arity() const { return Parameters_.size(); }
    TExprId Apply(TExpressionArena& destination, std::span<const TExprId> arguments,
        TSourcePosition position = {}) const;
private:
    std::shared_ptr<const TExpressionArena> Arena_;
    TExprId Root_;
    std::unordered_map<TColumnId, std::size_t> Parameters_;
};

// Value names and callable variable names omit '$'. Qualified callable names
// retain their exact spelling. SQL declarations and lexical bindings shadow
// these inputs normally. Values are closed bodies with no formal arguments.
struct TExternalBindings {
    std::unordered_map<std::string, TScalarBinding> Values;
    std::unordered_map<std::string, TScalarBinding> Callables;
};

} // namespace NKikimr::NKqp::NFrontend
