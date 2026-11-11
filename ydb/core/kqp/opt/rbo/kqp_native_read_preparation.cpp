#include "kqp_native_read_preparation.h"
#include "kqp_rbo.h"

namespace NKikimr::NKqp {

void PrepareNativeIndexReads(TOpRoot& root, std::span<const TNativeIndexRead> reads) {
    if (reads.empty()) return;
    // Local demand retains every expression definition, including subplans and
    // shared consumers. This is construction of the requested source, not an
    // extra pruning stage or a choice between candidate indexes.
    ComputePlanLiveness(root);
    THashMap<IOperator*, TIntrusivePtr<IOperator>> replacements;
    auto& registry = root.PlanProps.InfoUnitRegistry;
    for (const auto& [read, index] : reads) {
        Y_ENSURE(index.Table && index.Table.NativeCatalog && !index.MainKeys.empty());
        const auto& columns = index.Table;
        auto needed = GetLiveOut(read.Get());
        bool covering = true;
        for (const auto id : needed) covering &= columns.HasColumn(registry.Get(id).GetColumnName());
        if (covering) {
            read->TableRef = index.Table;
            read->GetColumns() = needed;
            continue;
        }
        // An adjacent filter whose entire input is in the index belongs to the
        // index side of this source construction. Keep its expression intact:
        // splitting predicates here would duplicate optimizer policy and could
        // change short-circuit/error behavior. The existing range rule still
        // chooses and constructs all bounds.
        TOpFilter* filter = nullptr;
        if (read->Parents.size() == 1 && read->Parents.front().first->Kind == EOperator::Filter) {
            auto* candidate = CastOperator<TOpFilter>(read->Parents.front().first);
            bool covered = candidate->GetUniqueRawInputIUs().IsSubsetOf(read->GetColumns());
            for (const auto id : candidate->GetUniqueRawInputIUs())
                covered &= columns.HasColumn(registry.Get(id).GetColumnName());
            if (covered) {
                filter = candidate;
                needed = GetLiveOut(filter);
            }
        }
        // The index key bindings belong to the lookup input. Its fetched row
        // defines the original SQL bindings, even when a primary key is selected.
        TUnorderedIUs keys;
        TOpTableLookup::TLookupKeys lookupKeys;
        THashMap<TString, TInfoUnitId> inputColumns;
        const auto inputColumn = [&](const TString& name) {
            const auto [it, inserted] = inputColumns.emplace(name, TInfoUnitId{});
            if (inserted) {
                it->second = registry.Add(TInfoUnit(read->Alias, name));
                keys.Add(it->second);
            }
            return it->second;
        };
        for (const auto& name : index.MainKeys) {
            Y_ENSURE(columns.HasColumn(name), "Index is missing a primary key column: " << name);
            lookupKeys.Append(inputColumn(name), name);
        }
        TSubstitutions renames;
        if (filter) for (const auto id : filter->GetUniqueRawInputIUs())
            renames.Add(id, inputColumn(registry.Get(id).GetColumnName()));
        TIntrusivePtr<IOperator> source = MakeIntrusive<TOpRead>(read->Alias, std::move(keys), read->StorageType, index.Table,
            nullptr, std::nullopt, std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, read->Pos);
        if (filter) source = MakeIntrusive<TOpFilter>(source, filter->Pos, filter->GetFilterExpression().ApplyRenames(renames));
        auto lookup = MakeIntrusive<TOpTableLookup>(source, read->Pos, read->TableRef, needed, std::move(lookupKeys));
        lookup->AllowNullKeys = true;
        replacements.emplace(filter ? static_cast<IOperator*>(filter) : read.Get(), std::move(lookup));
    }
    // Replace owning slots, including scalar-subquery roots and Replicate inputs.
    // The replacement owns a fresh read, so it cannot revisit the old source.
    TVector<TIntrusivePtr<IOperator>*> pending{&root.MutableChild(0)};
    for (const auto& [id, subplan] : root.PlanProps.Subplans)
        pending.push_back(&root.PlanProps.Subplans.MutablePlan(id));
    THashSet<IOperator*> visited;
    while (!pending.empty()) {
        auto& owner = *pending.back(); pending.pop_back();
        if (const auto replacement = replacements.find(owner.Get()); replacement != replacements.end())
            owner = replacement->second;
        if (!visited.insert(owner.Get()).second) continue;
        for (size_t child = 0; child < owner->GetChildCount(); ++child)
            pending.push_back(&owner->MutableChild(child));
    }
    for (const auto& item : IterateSubtreeWithSubplans(&root, root.PlanProps)) {
        item.Current->Props.ClearLogicalAnalysis();
        item.Current->Props.OutputIUs.reset();
        item.Current->GetOutputIUs();
    }
    root.ComputeParents();
}

} // namespace NKikimr::NKqp
