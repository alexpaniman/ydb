#include "kqp_native_write_preparation.h"
#include "kqp_native_frontend.h"

namespace NKikimr::NKqp {
namespace {

using NFrontend::ETypeKind;
using NFrontend::TType;

TIntrusivePtr<IOperator> RequireNoConflicts(TVector<TIntrusivePtr<IOperator>> conflicts,
    TPositionHandle pos, TPlanProps& props) {
    TUnionInputRow markers;
    for (auto& conflict : conflicts) {
        const auto marker = props.InfoUnitRegistry.AddGenerated("conflict_marker");
        TMapIUs columns;
        columns.Add(marker, MakeConstant("Uint64", "1", pos, &props));
        conflict = MakeIntrusive<TOpMap>(std::move(conflict), pos, std::move(columns));
        markers.Inputs.push_back(marker);
    }
    const auto row = props.InfoUnitRegistry.AddGenerated("conflict_row");
    const auto count = props.InfoUnitRegistry.AddGenerated("conflict_count");
    TUnionAllIUs columns(TUnionInputPolicy{conflicts.size()});
    columns.Add(row, std::move(markers));
    auto rows = MakeIntrusive<TOpUnionAll>(std::move(conflicts), pos, std::move(columns));
    TAggregationIUs traits;
    traits.Add(count, TOpAggregationTraits{row, "count"});
    auto aggregate = MakeIntrusive<TOpAggregate>(rows, std::move(traits), TOrderedIUs<>{}, EOpPhase::Undefined, false, pos);
    // COUNT(non-null marker) produces zero for empty input, including conflicts
    // on a NULL primary key. The gate emits exactly one row or no rows.
    return MakeIntrusive<TOpFilter>(aggregate, pos,
        MakeBinaryPredicate("==", MakeColumnAccess(count, TType{ETypeKind::Uint64}, pos, &props),
            MakeConstant("Uint64", "0", pos, &props)));
}

void CollectKeyConflicts(const TIntrusivePtr<TOpReplicate>& rows, const TRboTable& table,
    const TVector<TString>& names, const THashMap<TString, TInfoUnitId>& columns,
    const TRboTable::TColumns& types, bool skipNullKeys, TPositionHandle pos, TPlanProps& props,
    TVector<TIntrusivePtr<IOperator>>& conflicts) {
    Y_ENSURE(!names.empty());
    TIntrusivePtr<IOperator> input = rows;
    TOrderedIUs<> keys;
    TVector<TExpression> nonNull;
    for (const auto& name : names) {
        const auto key = rows->GetRebindings().At(columns.at(name));
        keys.Append(key);
        if (skipNullKeys) nonNull.push_back(MakeUnaryCallable("Exists", MakeColumnAccess(key, types.at(name), pos, &props)));
    }
    if (skipNullKeys) input = MakeIntrusive<TOpFilter>(input, pos, MakeConjunction(nonNull));

    // One grouping counts input duplicates and reduces storage probes to one
    // per distinct key. Both checks consume ports of the same grouped input.
    const auto row = props.InfoUnitRegistry.AddGenerated("insert_row");
    const auto count = props.InfoUnitRegistry.AddGenerated("key_count");
    TMapIUs marker;
    marker.Add(row, MakeConstant("Uint64", "1", pos, &props));
    auto counted = MakeIntrusive<TOpMap>(input, pos, std::move(marker));
    TAggregationIUs traits;
    traits.Add(count, TOpAggregationTraits{row, "count"});
    auto grouped = MakeIntrusive<TOpAggregate>(counted, std::move(traits), keys, EOpPhase::Undefined, false, pos);
    auto groups = TReplicate::Create(grouped, pos, props.InfoUnitRegistry);
    auto duplicates = groups->AddOutput();
    auto probe = groups->AddOutput();
    conflicts.push_back(MakeIntrusive<TOpFilter>(duplicates, pos,
        MakeBinaryPredicate(">", MakeColumnAccess(count, TType{ETypeKind::Uint64}, pos, &props),
            MakeConstant("Uint64", "1", pos, &props))));

    TOpTableLookup::TLookupKeys lookupKeys;
    for (size_t i = 0; i < names.size(); ++i) lookupKeys.Append(probe->GetRebindings().At(keys.Items()[i]), names[i]);
    const auto found = props.InfoUnitRegistry.Add(TInfoUnit("insert_conflict", names.front()));
    auto lookup = MakeIntrusive<TOpTableLookup>(probe, pos, table, TUnorderedIUs{found}, std::move(lookupKeys));
    lookup->AllowNullKeys = true;
    conflicts.push_back(std::move(lookup));
}

} // namespace

TIntrusivePtr<IOperator> PrepareInsertRevert(TIntrusivePtr<IOperator> input,
    const TOrderedIUs<TString>& columns, const TNativeTable& table,
    TPositionHandle pos, TPlanProps& props) {
    Y_ENSURE(table.Schema.WriteSchema && table.Table.NativeColumns);
    THashMap<TString, TInfoUnitId> source;
    for (const auto& [id, name] : columns.Items()) source.emplace(name, id);
    TVector<TString> primary;
    for (const auto& name : table.Schema.WriteSchema->Keys) primary.emplace_back(name);

    // The write and every check share the original rows, including volatile
    // expressions. A conflict suppresses the entire statement, not one row.
    auto rows = TReplicate::Create(std::move(input), pos, props.InfoUnitRegistry);
    auto insert = rows->AddOutput();
    TVector<TIntrusivePtr<IOperator>> conflicts;
    CollectKeyConflicts(rows->AddOutput(), table.Table, primary, source, *table.Table.NativeColumns,
        false, pos, props, conflicts);
    for (const auto& constraint : table.UniqueConstraints) {
        // INSERT binding supplies omitted nullable columns as NULL. Unique
        // indexes exclude any key containing NULL; primary keys retain them.
        CollectKeyConflicts(rows->AddOutput(), constraint.Table, constraint.Keys, source, *table.Table.NativeColumns,
            true, pos, props, conflicts);
    }
    auto gate = RequireNoConflicts(std::move(conflicts), pos, props);
    return MakeIntrusive<TOpJoin>(insert, gate, pos, "Cross", TJoinIUs{});
}

} // namespace NKikimr::NKqp
