#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_olap.h>
#include <ydb/core/kqp/provider/yql_kikimr_settings.h>

namespace NKikimr::NKqp {

namespace {

using namespace NKikimr;
using namespace NKikimr::NKqp;

bool IsSuitableToPushProjectionToColumnTables(IOperator* input) {
    if (input->Kind != EOperator::Map) {
        return false;
    }

    const auto filter = CastOperator<TOpMap>(input);
    const auto maybeRead = filter->GetInput().Get();
    return ((maybeRead->Kind == EOperator::Source) && (CastOperator<TOpRead>(maybeRead)->GetTableStorageType() == NYql::EStorageType::ColumnStorage) &&
            filter->HasSchema());
}

TIntrusivePtr<IOperator> PushNativeProjections(const TIntrusivePtr<TOpMap>& map, TPlanProps& props) {
    using namespace NFrontend;
    const auto read = CastOperator<TOpRead>(map->GetInput().Get());
    Y_ENSURE(!read->OlapProgram || read->OlapProgram.IsNative(), "Native read contains a legacy storage program");
    auto program = read->OlapProgram ? read->OlapProgram.Native() : TOlapProgram::TNative{};
    auto blocked = GetLiveOut(map.Get());
    blocked.IntersectWith(read->GetColumns());
    if (program.Predicate) blocked.UnionWith(program.Predicate->GetRawInputIUs());
    blocked.UnionWith(program.Projections.Keys());
    // The physical backend replaces storage fields by name. Two IDs reading
    // that field cannot independently retain its old and projected values.
    THashMap<TString, TInfoUnitId> storageNames;
    for (const auto id : read->GetColumns()) {
        const auto [it, inserted] = storageNames.emplace(props.InfoUnitRegistry.Get(id).GetColumnName(), id);
        if (!inserted) { blocked.Add(id); blocked.Add(it->second); }
    }
    struct TCandidate { TInfoUnitId Output; TInfoUnitId Column; TExprId Json; };
    TVector<TCandidate> candidates;
    THashMap<TInfoUnitId, std::string> paths;
    for (const auto& [output, element] : map->GetMapElements().Items()) {
        if (element.IsColumnAccess()) {
            blocked.Add(element.GetColumnAccess());
            continue;
        }
        const auto& expression = element.GetExpression();
        Y_ENSURE(expression.IsNative());
        const auto& arena = *expression.GetNativeArena();
        THashSet<TExprId> visited;
        TVector<TExprId> pending{expression.GetNativeRoot()}, json;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (!visited.insert(id).second) continue;
            const auto& node = arena[id];
            if (node.Op == EOpcode::JsonValue) json.push_back(id);
            pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
        }
        if (json.size() != 1 || !CanLowerNativeOlapProjection(arena, json.front())) {
            blocked.UnionWith(expression.GetRawInputIUs());
            continue;
        }
        const auto& value = arena[json.front()];
        const auto column = arena[value.Arguments[0]].ColumnId;
        if (!read->GetColumns().Contains(column)) {
            blocked.UnionWith(expression.GetRawInputIUs());
            continue;
        }
        // Every use outside this candidate still needs its original input.
        visited.clear(); pending = {expression.GetNativeRoot()};
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (id == json.front() || !visited.insert(id).second) continue;
            const auto& node = arena[id];
            if (node.Op == EOpcode::Column) blocked.Add(node.ColumnId);
            pending.insert(pending.end(), node.Arguments.begin(), node.Arguments.end());
        }
        const auto [it, inserted] = paths.emplace(column, value.Name);
        if (!inserted && it->second != value.Name) blocked.Add(column);
        candidates.push_back({output, column, json.front()});
    }
    auto elements = map->GetMapElements();
    bool changed = false;
    for (const auto& candidate : candidates) {
        if (blocked.Contains(candidate.Column)) continue;
        const auto& expression = map->GetMapElements().At(candidate.Output).GetExpression();
        const auto& arena = expression.GetNativeArena();
        if (!program.Projections.Find(candidate.Column)) program.Projections.Add(candidate.Column,
            TExpression::FromNative(arena, candidate.Json, &props, expression.Pos()));
        const auto replacement = arena->Column(candidate.Column, (*arena)[candidate.Json].Type);
        const auto root = arena->Rewrite(expression.GetNativeRoot(), [&](TExprId id) { return id == candidate.Json ? replacement : id; });
        elements.Replace(candidate.Output, TMapElement(TExpression::FromNative(arena, root, &props, expression.Pos())));
        changed = true;
    }
    if (!changed) return map;
    auto source = MakeIntrusive<TOpRead>(read->Alias, read->GetColumns(), read->StorageType, read->TableRef,
        TOlapProgram(std::move(program)), read->Limit, read->RangeInfo, read->OriginalPredicate, read->SortDir, read->Props, read->Pos);
    return MakeIntrusive<TOpMap>(source, map->Pos, std::move(elements));
}

} // anonymous namespace

bool TPushOlapProjectionRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Map &&
        input->GetChildren().front()->Kind == EOperator::Source;
}

TIntrusivePtr<IOperator> TPushOlapProjectionRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    if (!(ctx.KqpCtx.Config->HasOptEnableOlapPushdown() && ctx.KqpCtx.Config->GetEnableOlapPushdownProjections())) {
        return input;
    }

    if (!IsSuitableToPushProjectionToColumnTables(input.get())) {
        return input;
    }

    const auto map = CastOperator<TOpMap>(input);
    return PushNativeProjections(map, props);
}

} // namespace NKikimr::NKqp
