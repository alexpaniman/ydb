#include "execution.h"

#include <algorithm>
#include <unordered_set>

namespace NKikimr::NKqp::NFrontend {
namespace {

// Relations, scalar DAGs and scalar-subquery producers form one dependency
// graph. Considering all three avoids missing sharing behind an expression.
struct TNode {
    std::vector<std::size_t> Inputs;
    bool Repeatable = true;
};

std::vector<TNode> Dependencies(const TBoundProgram& program) {
    const auto offset = program.Relations.size();
    std::vector<TNode> nodes(offset + program.Expressions.Size());
    for (std::size_t i = 0; i < offset; ++i) {
        const auto& relation = program.Relations[i];
        auto& inputs = nodes[i].Inputs;
        inputs.assign(relation.Inputs.begin(), relation.Inputs.end());
        for (const auto& definition : relation.Definitions) inputs.push_back(offset + definition.Expression);
        for (const auto expression : {relation.Predicate, relation.Count, relation.Offset})
            if (expression) inputs.push_back(offset + *expression);
    }
    for (TExprId i = 0; i < program.Expressions.Size(); ++i) {
        const auto& expression = program.Expressions[i];
        auto& node = nodes[offset + i];
        node.Repeatable = expression.Repeatable;
        for (auto child : expression.Arguments) node.Inputs.push_back(offset + child);
        if (expression.Op == EOpcode::Column) {
            if (auto subplan = program.Subplans.find(expression.ColumnId); subplan != program.Subplans.end())
                node.Inputs.push_back(subplan->second.Root);
        }
    }
    return nodes;
}

// Visit shared nodes once, without recursion proportional to SQL graph depth.
std::vector<std::size_t> DependencyOrder(const TBoundProgram& program, const std::vector<TNode>& nodes) {
    std::vector<std::uint8_t> state(nodes.size());
    std::vector<std::size_t> order;
    std::vector<std::pair<std::size_t, bool>> pending;
    for (const auto& result : program.Results) {
        pending.emplace_back(result.Root, false);
        while (!pending.empty()) {
            const auto [id, ready] = pending.back(); pending.pop_back();
            if (state.at(id) == 2) continue;
            if (ready) { state[id] = 2; order.push_back(id); continue; }
            if (state[id] == 1) throw TFrontendError({}, "cyclic native execution dependency");
            state[id] = 1;
            pending.emplace_back(id, true);
            for (auto input = nodes[id].Inputs.rbegin(); input != nodes[id].Inputs.rend(); ++input)
                pending.emplace_back(*input, false);
        }
    }
    return order;
}

struct TMaterialization {
    std::string Name;
    std::vector<TColumnId> Columns;
};

} // namespace

TExecutionPlan PrepareExecution(TCompiledProgram compiled) {
    TExecutionPlan plan{std::move(compiled).Release(), {}};
    auto& program = plan.Program;
    const auto relationCount = program.Relations.size();
    // A single statement without named producers needs no scheduling graph.
    if (program.Results.size() == 1 && std::none_of(program.Relations.begin(), program.Relations.end(),
            [](const auto& relation) { return relation.Kind == ERelation::Reuse; })) {
        auto& result = program.Results.front();
        const auto phase = program.Relations[result.Root].Effect ? 1u : 0u;
        plan.Roots.push_back({std::move(result), phase, {}});
        program.Results.clear();
        return plan;
    }
    auto nodes = Dependencies(program);
    const auto order = DependencyOrder(program, nodes);
    std::vector<unsigned> uses(relationCount);
    for (auto id : order) if (id < relationCount && program.Relations[id].Kind == ERelation::Reuse)
        ++uses[program.Relations[id].Inputs.at(0)];
    for (auto id : order)
        for (auto input : nodes[id].Inputs) nodes[id].Repeatable &= nodes[input].Repeatable;

    constexpr auto unknown = std::numeric_limits<std::size_t>::max();
    constexpr auto shared = unknown - 1;
    std::vector<std::size_t> owners(nodes.size(), unknown);
    const auto merge = [&](std::size_t id, std::size_t owner) {
        if (owners[id] == unknown) owners[id] = owner;
        else if (owners[id] != owner) owners[id] = shared;
    };
    for (std::size_t i = 0; i < program.Results.size(); ++i) merge(program.Results[i].Root, nodes.size() + i);
    std::unordered_map<TRelId, TMaterialization> materialized;
    std::unordered_set<std::string> names;
    for (const auto& [name, type] : program.Parameters) names.insert(name);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto id = *it;
        if (id < relationCount && uses[id] &&
                (owners[id] == shared || (uses[id] > 1 && !nodes[id].Repeatable))) {
            auto name = "_native_query_" + std::to_string(id);
            while (!names.insert(name).second) name += '_';
            materialized.emplace(id, TMaterialization{std::move(name), {}});
            // A materialized producer owns its dependencies. Nested producers
            // used only here do not need another transaction just because the
            // outer producer has several consumers.
            owners[id] = id;
        }
        for (auto input : nodes[id].Inputs) merge(input, owners[id]);
    }

    for (TRelId i = 0; i < relationCount; ++i) {
        if (program.Relations[i].Kind != ERelation::Reuse) continue;
        const auto producer = program.Relations[i].Inputs.at(0);
        const auto found = materialized.find(producer);
        if (found == materialized.end()) continue;
        auto& binding = found->second;
        TRelation parameter{ERelation::Parameter, program.Relations[i].Position};
        parameter.Table = binding.Name;
        std::vector<TDefinition> definitions;
        const bool first = binding.Columns.empty();
        for (const auto& definition : program.Relations[i].Definitions) {
            const auto& input = program.Expressions[definition.Expression];
            if (input.Op != EOpcode::Column) throw TFrontendError(input.Position, "invalid named-query binding");
            const auto column = input.ColumnId;
            const auto type = input.Type;
            const auto pos = input.Position;
            if (first) binding.Columns.push_back(column);
            const auto local = static_cast<TColumnId>(program.Columns.size());
            program.Columns.push_back({"_c" + std::to_string(column), {}, type});
            parameter.Columns.push_back(local);
            definitions.push_back({definition.Column, program.Expressions.Column(local, type, pos)});
        }
        auto& consumer = program.Relations[i];
        consumer.Kind = ERelation::Map;
        consumer.Definitions = std::move(definitions);
        consumer.Inputs = {static_cast<TRelId>(program.Relations.size())};
        program.Relations.push_back(std::move(parameter));
    }
    // Emit a shared producer at its first use, after preceding statements.
    // The original graph retains the producer edges removed by parameterization.
    // Visit each node once; its phase is the earliest phase of its consumers.
    std::vector<std::uint64_t> phases(nodes.size());
    std::vector<bool> visited(nodes.size());
    std::unordered_map<std::string, std::uint64_t> lastWrite;
    std::uint64_t currentPhase = 0;
    bool currentEffects = false;
    const auto emit = [&](TQueryResult result, std::uint64_t minimum, std::optional<std::string> name) {
        const auto& relation = program.Relations[result.Root];
        const bool effects = bool(relation.Effect);
        auto phase = std::max(currentPhase, minimum);
        if (phase == currentPhase && effects != currentEffects) ++phase;
        if (effects) {
            // Consecutive writes to one table can depend on rows read by
            // effect expansion (conflict checks and RETURNING, for example).
            if (const auto previous = lastWrite.find(relation.Table); previous != lastWrite.end())
                phase = std::max(phase, previous->second + 1);
            lastWrite[relation.Table] = phase;
        }
        currentPhase = phase;
        currentEffects = effects;
        plan.Roots.push_back({std::move(result), phase, std::move(name)});
        return phase;
    };
    std::vector<std::pair<std::size_t, bool>> pending;
    for (auto& result : program.Results) {
        pending.emplace_back(result.Root, false);
        while (!pending.empty()) {
            const auto [id, ready] = pending.back(); pending.pop_back();
            if (visited[id]) continue;
            if (!ready) {
                pending.emplace_back(id, true);
                for (auto input = nodes[id].Inputs.rbegin(); input != nodes[id].Inputs.rend(); ++input)
                    pending.emplace_back(*input, false);
                continue;
            }
            visited[id] = true;
            for (auto input : nodes[id].Inputs)
                phases[id] = std::max(phases[id], phases[input] + (materialized.contains(input) ? 1 : 0));
            if (id < relationCount && program.Relations[id].Kind == ERelation::Read) {
                if (const auto previous = lastWrite.find(program.Relations[id].Table); previous != lastWrite.end())
                    phases[id] = std::max(phases[id], previous->second + 1);
            }
            if (const auto found = materialized.find(id); found != materialized.end()) {
                TQueryResult producer{static_cast<TRelId>(id), {}};
                for (auto column : found->second.Columns)
                    producer.Columns.push_back({column, "_c" + std::to_string(column), program.Columns[column].Type});
                phases[id] = emit(std::move(producer), phases[id], found->second.Name);
            }
        }
        const auto minimum = phases[result.Root];
        emit(std::move(result), minimum, {});
    }
    program.Results.clear();
    return plan;
}

} // namespace NKikimr::NKqp::NFrontend
