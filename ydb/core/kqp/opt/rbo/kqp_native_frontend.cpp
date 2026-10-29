#include "kqp_native_frontend.h"
#include "frontend_lowering.h"
#include "frontend/compiler/compiler.h"

#include <unordered_map>

namespace NKikimr::NKqp {
namespace {
TString String(std::string_view text) { return TString(text.data(), text.size()); }

NYql::TPositionHandle Position(NFrontend::TSourcePosition pos, NYql::TExprContext& ctx) {
    return ctx.AppendPosition(NYql::TPosition(pos.Column, pos.Line));
}

TString JoinName(NFrontend::EJoinKind kind) {
    using K = NFrontend::EJoinKind;
    switch (kind) {
        case K::Inner: return "Inner";
        case K::Cross: return "Cross";
        case K::Left: return "Left";
        case K::Right: return "Right";
        case K::Full: return "Full";
        case K::LeftSemi: return "LeftSemi";
        case K::RightSemi: return "RightSemi";
        case K::LeftOnly: return "LeftOnly";
        case K::RightOnly: return "RightOnly";
    }
    Y_UNREACHABLE();
}
} // namespace

TVector<TIntrusivePtr<TOpRoot>> CompileNativeToRbo(std::string_view sql,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx,
    const NFrontend::TBindSettings& bindSettings, const NFrontend::TParseSettings& parseSettings) {
    std::unordered_map<std::string, TNativeTable> tables;
    NFrontend::TCompilerSettings settings;
    settings.Binding = bindSettings;
    settings.Parsing = parseSettings;
    auto compiled = NFrontend::CompileNative(sql, [&](std::string_view name, NFrontend::TSourcePosition pos) {
        auto it = tables.find(std::string(name));
        if (it == tables.end()) {
            auto table = resolve(name, pos);
            if (!table.Table) throw NFrontend::TFrontendError(pos, "table resolver returned no table identity");
            auto schema = table.Schema;
            // The bound relation uses the resolved path, not its source alias.
            tables.emplace(schema.Name, table);
            tables.emplace(std::string(name), std::move(table));
            return schema;
        }
        return it->second.Schema;
    }, settings);
    return ConvertNativeToRbo(std::move(compiled.Program.value()), [&](std::string_view name, NFrontend::TSourcePosition) {
        return tables.at(std::string(name));
    }, ctx);
}

TVector<TIntrusivePtr<TOpRoot>> ConvertNativeToRbo(NFrontend::TBoundProgram program,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx) {
    std::unordered_map<std::string, TNativeTable> tables;
    for (const auto& relation : program.Relations) if (relation.Kind == NFrontend::ERelation::Read && !tables.contains(relation.Table)) {
        auto table = resolve(relation.Table, relation.Position);
        if (!table.Table) throw NFrontend::TFrontendError(relation.Position, "table resolver returned no table identity");
        tables.emplace(relation.Table, std::move(table));
    }
    auto arena = std::make_shared<NFrontend::TExpressionArena>(std::move(program.Expressions));
    TVector<TIntrusivePtr<TOpRoot>> roots;
    for (const auto& result : program.Results) {
        TPlanProps props;
        props.NativeExpressions = true;
        for (const auto& column : program.Columns) props.InfoUnitRegistry.Add(TInfoUnit(String(column.Qualifier), String(column.Name)));
        auto expression = [&](NFrontend::TExprId id) {
            return TExpression::FromNative(arena, id, &ctx, &props, Position((*arena)[id].Position, ctx));
        };
        std::unordered_map<NFrontend::TRelId, TIntrusivePtr<IOperator>> converted;
        std::vector<std::pair<NFrontend::TRelId, bool>> pending{{result.Root, false}};
        while (!pending.empty()) {
            const auto [id, ready] = pending.back(); pending.pop_back();
            if (converted.contains(id)) continue;
            const auto& relation = program.Relations.at(id);
            if (!ready) {
                pending.emplace_back(id, true);
                for (auto it = relation.Inputs.rbegin(); it != relation.Inputs.rend(); ++it) pending.emplace_back(*it, false);
                continue;
            }
            auto pos = Position(relation.Position, ctx);
            TIntrusivePtr<IOperator> op;
            auto input = [&](std::size_t index = 0) { return converted.at(relation.Inputs.at(index)); };
            using R = NFrontend::ERelation;
            switch (relation.Kind) {
                case R::Unit: op = MakeIntrusive<TOpEmptySource>(pos); break;
                case R::Read: {
                    const auto& table = tables.at(relation.Table);
                    TUnorderedIUs columns; for (auto column : relation.Columns) columns.Add(column);
                    op = MakeIntrusive<TOpRead>("", std::move(columns), table.Storage, table.Table, nullptr, nullptr,
                        std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, pos);
                    break;
                }
                case R::Map: {
                    TMapIUs definitions;
                    for (const auto& definition : relation.Definitions) definitions.Add(definition.Column, TMapElement(expression(definition.Expression)));
                    op = MakeIntrusive<TOpMap>(input(), pos, std::move(definitions)); break;
                }
                case R::Filter: op = MakeIntrusive<TOpFilter>(input(), pos, expression(*relation.Predicate)); break;
                case R::Join: {
                    TJoinIUs keys;
                    TVector<TExpression> filters;
                    auto left = input(), right = input(1);
                    if (relation.Predicate) {
                        for (auto predicate : expression(*relation.Predicate).SplitConjunct()) {
                            if (predicate.MaybeEquiJoinCondition()) {
                                TEquiJoinCondition equality(predicate);
                                auto a = equality.GetLeftIU(), b = equality.GetRightIU();
                                if (left->GetOutputIUs().Contains(b) && right->GetOutputIUs().Contains(a)) std::swap(a, b);
                                if (left->GetOutputIUs().Contains(a) && right->GetOutputIUs().Contains(b)) {
                                    keys.Add({a, b, false}); continue;
                                }
                            }
                            filters.push_back(std::move(predicate));
                        }
                    }
                    op = MakeIntrusive<TOpJoin>(left, right, pos, JoinName(relation.JoinKind), std::move(keys), filters); break;
                }
                case R::Aggregate: {
                    bool distinct = false;
                    for (const auto& aggregate : relation.Aggregates) distinct |= aggregate.Distinct;
                    if (distinct && relation.Aggregates.size() != 1) throw NFrontend::TFrontendError(relation.Position, "native mixed DISTINCT aggregates are not supported");
                    TOrderedIUs<> keys; for (auto key : relation.GroupKeys) keys.Append(key);
                    TAggregationIUs aggregates;
                    for (const auto& aggregate : relation.Aggregates) aggregates.Add(aggregate.Output,
                        TOpAggregationTraits{aggregate.Input, String(aggregate.Function), aggregate.Distinct});
                    op = MakeIntrusive<TOpAggregate>(input(), std::move(aggregates), std::move(keys), EOpPhase::Undefined, false, pos); break;
                }
                case R::Sort: {
                    TSortIUs keys; for (const auto& key : relation.SortKeys) keys.Append(key.Column, TSortOrder{!key.Descending, !key.Descending});
                    op = MakeIntrusive<TOpSort>(input(), pos, std::move(keys), std::nullopt); break;
                }
                case R::Limit: {
                    if (relation.Offset) op = MakeIntrusive<TOpLimit>(input(), pos, expression(*relation.Count), expression(*relation.Offset), EOpPhase::Undefined);
                    else op = MakeIntrusive<TOpLimit>(input(), pos, expression(*relation.Count), EOpPhase::Undefined);
                    break;
                }
                case R::UnionAll: {
                    TVector<TIntrusivePtr<IOperator>> inputs; for (auto child : relation.Inputs) inputs.push_back(converted.at(child));
                    TUnionAllIUs columns(TUnionInputPolicy{inputs.size()});
                    for (std::size_t i = 0; i < relation.Columns.size(); ++i) {
                        TUnionInputRow row; for (const auto& branch : relation.UnionInputs) row.Inputs.push_back(branch[i]);
                        columns.Add(relation.Columns[i], std::move(row));
                    }
                    op = MakeIntrusive<TOpUnionAll>(std::move(inputs), pos, std::move(columns), false); break;
                }
            }
            op->GetOutputIUs();
            converted.emplace(id, std::move(op));
        }
        TOrderedIUs<TString> columns;
        for (const auto& column : result.Columns) columns.Append(column.Column, String(column.Name));
        auto root = MakeIntrusive<TOpRoot>(converted.at(result.Root), converted.at(result.Root)->Pos, std::move(columns));
        root->PlanProps = std::move(props);
        for (const auto& item : *root) item.Current->BindExpressionPlanProps(&root->PlanProps);
        root->RecomputeOutputIUsSubtree(); root->ComputeParents(); roots.push_back(std::move(root));
    }
    return roots;
}

} // namespace NKikimr::NKqp
