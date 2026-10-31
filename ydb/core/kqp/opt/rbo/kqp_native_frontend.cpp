#include "kqp_native_frontend.h"
#include "frontend_lowering.h"
#include "frontend/frontend.h"

#include <unordered_map>
#include <unordered_set>

namespace NKikimr::NKqp {
namespace {
TString String(std::string_view text) { return TString(text.data(), text.size()); }

TString AggregateName(std::string_view name) {
    // This existing backend trait computes sqrt(M2 / (count - 1)).
    return name == "stddev_samp" ? TString("variance_1_1") : String(name);
}

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

TVector<TIntrusivePtr<TOpRoot>> ConvertNativeToRbo(NFrontend::TCompiledProgram compiled,
    const TNativeTableResolver& resolve, NYql::TExprContext& ctx) {
    auto program = std::move(compiled).Release();
    std::unordered_map<std::string, TNativeTable> tables;
    for (const auto& relation : program.Relations) if (relation.Kind == NFrontend::ERelation::Read && !tables.contains(relation.Table)) {
        auto table = resolve(relation.Table, relation.Position);
        if (!table.Table) throw NFrontend::TFrontendError(relation.Position, "table resolver returned no table identity");
        tables.emplace(relation.Table, std::move(table));
    }
    auto arena = std::make_shared<NFrontend::TExpressionArena>(std::move(program.Expressions));
    std::unordered_map<NFrontend::TRelId, NFrontend::TColumnId> subplanBindings;
    for (const auto& [binding, subplan] : program.Subplans) subplanBindings.emplace(subplan.Root, binding);
    TVector<TIntrusivePtr<TOpRoot>> roots;
    for (const auto& result : program.Results) {
        TPlanProps props;
        props.NativeExpressions = arena;
        for (const auto& column : program.Columns) props.InfoUnitRegistry.Add(TInfoUnit(String(column.Qualifier), String(column.Name)));
        auto expression = [&](NFrontend::TExprId id) {
            return TExpression::FromNative(arena, id, &ctx, &props, Position((*arena)[id].Position, ctx));
        };
        std::unordered_map<NFrontend::TRelId, TIntrusivePtr<IOperator>> converted;
        std::vector<std::pair<NFrontend::TRelId, bool>> pending{{result.Root, false}};
        std::unordered_set<NFrontend::TExprId> visitedExpressions;
        auto scheduleExpression = [&](NFrontend::TExprId root) {
            std::vector<NFrontend::TExprId> expressions{root};
            while (!expressions.empty()) {
                const auto id = expressions.back(); expressions.pop_back();
                if (!visitedExpressions.insert(id).second) continue;
                const auto& node = (*arena)[id];
                expressions.insert(expressions.end(), node.Arguments.begin(), node.Arguments.end());
                if (node.Op == NFrontend::EOpcode::Column) {
                    if (const auto call = program.Subplans.find(node.ColumnId); call != program.Subplans.end())
                        pending.emplace_back(call->second.Root, false);
                }
            }
        };
        while (!pending.empty()) {
            const auto [id, ready] = pending.back(); pending.pop_back();
            if (converted.contains(id)) continue;
            const auto& relation = program.Relations.at(id);
            if (!ready) {
                pending.emplace_back(id, true);
                for (auto it = relation.Inputs.rbegin(); it != relation.Inputs.rend(); ++it) pending.emplace_back(*it, false);
                for (const auto& definition : relation.Definitions) scheduleExpression(definition.Expression);
                if (relation.Predicate) scheduleExpression(*relation.Predicate);
                if (relation.Count) scheduleExpression(*relation.Count);
                if (relation.Offset) scheduleExpression(*relation.Offset);
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
                    op = MakeIntrusive<TOpRead>("", std::move(columns), table.Storage, table.Table, nullptr, std::nullopt,
                        std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, pos);
                    break;
                }
                case R::Map: {
                    TMapIUs definitions;
                    for (const auto& definition : relation.Definitions) definitions.Add(definition.Column, TMapElement(expression(definition.Expression)));
                    op = MakeIntrusive<TOpMap>(input(), pos, std::move(definitions)); break;
                }
                case R::Capture: {
                    TDependencyIUs captures;
                    for (const auto& capture : relation.Captures) captures.Add(capture.Local,
                        TCapturedIU{capture.Outer, ExportNativeType(program.Columns.at(capture.Local).Type, ctx, arena.get())});
                    op = MakeIntrusive<TOpAddDependencies>(input(), pos, std::move(captures)); break;
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
                    if (relation.GroupingSets.empty()) {
                        TOrderedIUs<> keys; for (auto key : relation.GroupKeys) keys.Append(key);
                        TAggregationIUs aggregates;
                        for (const auto& aggregate : relation.Aggregates) aggregates.Add(aggregate.Output,
                            TOpAggregationTraits{aggregate.Input, AggregateName(aggregate.Function), aggregate.Distinct, false,
                                aggregate.Function == "avg" ? std::optional{program.Columns[aggregate.Output].Type} : std::nullopt});
                        op = MakeIntrusive<TOpAggregate>(input(), std::move(aggregates), std::move(keys), EOpPhase::Undefined, false, pos);
                        break;
                    }
                    // Lower once to executable branches. A GroupingSets wrapper
                    // requires an untouched aggregate child; expanding DISTINCT
                    // below it would change that child's meaning and structure.
                    const bool multiple = relation.GroupingSets.size() > 1;
                    auto& registry = props.InfoUnitRegistry;
                    auto hub = multiple ? TReplicate::Create(input(), pos, registry) : TIntrusivePtr<TReplicate>{};
                    TVector<TIntrusivePtr<IOperator>> branches;
                    TMappedIUs<TUnionInputRow> rows;
                    for (auto output : relation.Columns) rows.Add(output);
                    for (const auto& set : relation.GroupingSets) {
                        auto port = hub ? hub->AddOutput() : nullptr;
                        const auto* renames = port && !port->IsPrimary() ? &port->GetRebindings() : nullptr;
                        auto rebind = [&](TInfoUnitId source) { return renames ? renames->At(source) : source; };
                        const std::unordered_set<NFrontend::TColumnId> present(set.begin(), set.end());
                        TOrderedIUs<> branchKeys;
                        TMappedIUs<TInfoUnitId> values;
                        std::unordered_map<NFrontend::TColumnId, NFrontend::TType> types;
                        for (auto key : set) {
                            branchKeys.Append(rebind(key)); values.Add(key, rebind(key));
                            types.emplace(key, program.Columns[key].Type);
                        }
                        TAggregationIUs traits;
                        for (const auto& aggregate : relation.Aggregates) {
                            const auto value = registry.AddCopy(aggregate.Output);
                            values.Add(aggregate.Output, value);
                            auto type = program.Columns[aggregate.Output].Type;
                            if (set.empty() && aggregate.Function != "count") type = type.Optional();
                            traits.Add(value, TOpAggregationTraits{rebind(aggregate.Input), AggregateName(aggregate.Function), aggregate.Distinct, false,
                                aggregate.Function == "avg" ? std::optional{type} : std::nullopt});
                            types.emplace(aggregate.Output, type);
                        }
                        TIntrusivePtr<IOperator> branch = MakeIntrusive<TOpAggregate>(port ? port : input(),
                            std::move(traits), std::move(branchKeys), EOpPhase::Undefined, false, pos);
                        TMapIUs padding;
                        auto add = [&](TInfoUnitId output, NFrontend::TExprId value) {
                            const auto column = multiple ? registry.AddCopy(output) : output;
                            padding.Add(column, TMapElement(expression(value)));
                            rows.At(output).Inputs.push_back(column);
                        };
                        for (const auto& [output, source] : relation.GroupingColumns) {
                            const auto type = program.Columns[output].Type;
                            const auto* value = values.Find(source);
                            auto scalar = value ? arena->Column(*value, types.at(source), relation.Position)
                                : arena->Literal(type, std::monostate{}, relation.Position);
                            add(output, arena->Convert(scalar, type, relation.Position));
                        }
                        for (const auto& [output, key] : relation.GroupingIndicators)
                            add(output, arena->Literal({NFrontend::ETypeKind::Uint64}, std::uint64_t{present.contains(key) ? 0u : 1u}, relation.Position));
                        if (!padding.Keys().Empty()) branch = MakeIntrusive<TOpMap>(branch, pos, std::move(padding));
                        branches.push_back(std::move(branch));
                    }
                    if (multiple) {
                        TUnionAllIUs outputs(TUnionInputPolicy{branches.size()});
                        for (auto output : relation.Columns) outputs.Add(output, std::move(rows.At(output)));
                        op = MakeIntrusive<TOpUnionAll>(std::move(branches), pos, std::move(outputs), false);
                    } else op = std::move(branches.front());
                    break;
                }
                case R::Sort: {
                    TSortIUs keys; for (const auto& key : relation.SortKeys) keys.Append(key.Column, TSortOrder{!key.Descending, !key.Descending});
                    op = MakeIntrusive<TOpSort>(input(), pos, std::move(keys), std::nullopt); break;
                }
                case R::Window: {
                    TWindowIUs functions;
                    for (const auto& function : relation.WindowFunctions) {
                        TOrderedIUs<> arguments;
                        if (function.Input) arguments.Append(*function.Input);
                        functions.Add(function.Output, TOpWindowFunc{String(function.Function),
                            function.Input ? EWindowFuncKind::Aggregate : EWindowFuncKind::Native, std::move(arguments)});
                    }
                    TOrderedIUs<> partitions; for (auto key : relation.GroupKeys) partitions.Append(key);
                    TSortIUs order; for (const auto& key : relation.SortKeys) order.Append(key.Column, TSortOrder{!key.Descending, !key.Descending});
                    auto bound = [](NFrontend::EWindowBound kind) {
                        using B = NFrontend::EWindowBound;
                        switch (kind) {
                            case B::UnboundedPreceding: return EWindowFrameBound::UnboundedPreceding;
                            case B::Preceding: return EWindowFrameBound::Preceding;
                            case B::CurrentRow: return EWindowFrameBound::CurrentRow;
                            case B::Following: return EWindowFrameBound::Following;
                            case B::UnboundedFollowing: return EWindowFrameBound::UnboundedFollowing;
                        }
                        Y_UNREACHABLE();
                    };
                    const auto& frame = relation.WindowFrame;
                    Y_ENSURE(frame.Type == NFrontend::EWindowFrame::Rows || frame.Type == NFrontend::EWindowFrame::Range);
                    op = MakeIntrusive<TOpWindow>(input(), pos, std::move(functions), std::move(partitions), std::move(order),
                        TOpWindowFrame{frame.Type == NFrontend::EWindowFrame::Rows ? EWindowFrameType::Rows : EWindowFrameType::Range,
                            bound(frame.Begin), frame.BeginOffset, bound(frame.End), frame.EndOffset});
                    break;
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
            converted.emplace(id, op);
            if (const auto call = subplanBindings.find(id); call != subplanBindings.end()) {
                const auto& subplan = program.Subplans.at(call->second);
                const auto kind = subplan.Kind == NFrontend::ESubquery::Exists ? ESubplanType::EXISTS :
                    subplan.Kind == NFrontend::ESubquery::In ? ESubplanType::IN_SUBPLAN : ESubplanType::EXPR;
                TOrderedIUs<> tuple;
                if (subplan.Lookup) tuple.Append(*subplan.Lookup);
                props.Subplans.Add(call->second, std::move(op), kind, std::move(tuple),
                    kind != ESubplanType::EXISTS ? std::optional<TInfoUnitId>{subplan.Result} : std::nullopt);
                props.Subplans.RefreshDependencies(call->second);
            }
        }
        TOrderedIUs<TString> columns;
        for (const auto& column : result.Columns) columns.Append(column.Column, String(column.Name));
        auto root = MakeIntrusive<TOpRoot>(converted.at(result.Root), converted.at(result.Root)->Pos, std::move(columns));
        root->PlanProps = std::move(props);
        for (const auto& item : IterateSubtreeWithSubplans(root.Get(), root->PlanProps)) item.Current->BindExpressionPlanProps(&root->PlanProps);
        root->RecomputeOutputIUsSubtree(); root->ComputeParents(); roots.push_back(std::move(root));
    }
    return roots;
}

} // namespace NKikimr::NKqp
