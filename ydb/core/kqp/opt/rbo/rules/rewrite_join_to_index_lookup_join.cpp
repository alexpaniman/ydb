#include <ydb/core/kqp/opt/rbo/rules/kqp_rules_include.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_lookup_join.h>

namespace NKikimr {
namespace NKqp {

using namespace NYql;
using namespace NYql::NNodes;

bool TRewriteJoinToIndexLookupJoinRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Join;
}

TIntrusivePtr<IOperator> TRewriteJoinToIndexLookupJoinRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                               TPlanProps& props) {
    auto join = CastOperator<TOpJoin>(input);
    if (join->Props.JoinAlgo && *join->Props.JoinAlgo != EJoinAlgoType::LookupJoin) {
        return input;
    }
    const auto prepared = PrepareLookupJoin(*join, ctx.KqpCtx, props.InfoUnitRegistry, ctx.ExprCtx);
    if (!prepared) {
        return input;
    }
    const auto joinKind = GetValidJoinKind(join->JoinKind);
    auto* read = prepared->RightSide.Read.Get();
    TIntrusivePtr<TOpRead> rewrittenRead;
    const auto* target = &prepared->Target;
    const auto& fetchedRowFilter = prepared->FetchedRowFilter;
    const auto& tableMeta = target->Metadata;
    if (TKqpTable(read->GetTable()).Path().Value() != tableMeta->Name) {
        auto rightOriginalType = read->Type;
        rewrittenRead = MakeIntrusive<TOpRead>(read->Alias, read->GetColumns(), read->GetTableStorageType(),
                                               BuildTableCallable(*tableMeta, read->Pos, ctx.ExprCtx), nullptr, read->Limit,
                                               std::nullopt, std::nullopt, ESortDir::None, read->Props, read->Pos);
        read = rewrittenRead.get();
        read->Type = rightOriginalType;
    }

    const auto table = TKqpTable(read->GetTable());
    const TOpRead::TPointPrefix* pointPrefix = target->PointPrefix;
    size_t pointPrefixLen = pointPrefix ? pointPrefix->Columns.size() : 0;

    TOpTableLookup::TLookupKeys lookupKeys;
    for (const auto& key : prepared->LookupKeys) {
        lookupKeys.Append(key.LeftIU, key.Column);
    }

    std::optional<TOpTableLookup::TLookupKeyPrefix> prefix;
    if (pointPrefixLen != 0) {
        TOpTableLookup::TLookupKeyPrefix keyPrefix;
        keyPrefix.Points = pointPrefix->Points;
        keyPrefix.PointsItemType = pointPrefix->PointsItemType;
        keyPrefix.Columns = pointPrefix->Columns;
        for (const auto& key : prepared->PrefixKeys) {
            keyPrefix.Equalities.Append(key.LeftIU, key.Column);
        }
        prefix = std::move(keyPrefix);
    }

    TPairedIUs residualJoinKeys;
    for (const auto& key : prepared->ResidualKeys) {
        residualJoinKeys.Add(key.LeftIU, key.RightIU);
    }

    YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Rewriting a " << joinKind << " join into an index lookup join of "
                                 << table.Path().StringValue();

    if (target->MainTable) {
        // The index is not covering: the index lookup finds primary keys of the matching rows, and the main table
        // lookup fetches the rows by them and applies the whole read predicate to them.
        Y_ENSURE(joinKind == "Inner", "A lookup join by a non-covering index is supported for inner joins only");
        const auto& mainKeyColumns = target->MainTable->KeyColumnNames;
        const auto& mainColumns = target->MainTable->Columns;
        const bool mainKeyNotNull = std::all_of(mainKeyColumns.begin(), mainKeyColumns.end(), [&](const TString& column) {
            const auto it = mainColumns.find(column);
            return it != mainColumns.end() && it->second.NotNull;
        });

        // The index lookup produces the main table primary key; each column gets a fresh definition whose
        // storage name is the primary key column, because that is what the lookup returns.
        TUnorderedIUs indexOutputs;
        TOpTableLookup::TLookupKeys mainLookupKeys;
        for (const auto& column : mainKeyColumns) {
            const auto id = props.InfoUnitRegistry.Add(TInfoUnit(read->Alias, column));
            indexOutputs.Add(id);
            mainLookupKeys.Append(id, column);
        }

        auto indexLookup = MakeIntrusive<TOpTableLookup>(join->GetLeftInput(), join->Pos, read->GetTable(), std::move(indexOutputs),
                                                         std::move(lookupKeys), joinKind, std::nullopt, prefix);

        const auto mainTableCallable = BuildTableCallable(*target->MainTable, read->Pos, ctx.ExprCtx);
        if (mainKeyNotNull) {
            // The index lookup feeds the main table lookup directly, and a single lookup join consumes the result.
            // A row without a match in the index comes with a missing key, which is a key of nulls for the lookup:
            // the primary key has no nulls, so such a key is not looked up.
            auto mainLookup = MakeIntrusive<TOpTableLookup>(std::move(indexLookup), join->Pos, mainTableCallable,
                                                            read->GetColumns(), std::move(mainLookupKeys), joinKind,
                                                            fetchedRowFilter, std::nullopt, std::move(residualJoinKeys));
            mainLookup->KeysFromInputLookup = true;
            return MakeIntrusive<TOpIndexLookupJoin>(std::move(mainLookup), join->Pos, joinKind, join->JoinKeys);
        }

        // The primary key can have nulls, which the main table lookup has to allow, so a key of a row without a
        // match in the index cannot be told apart. A lookup join drops such rows before the main table lookup.
        // This join only drops the rows which found nothing in the index; the index lookup already
        // enforced the join condition. It has no keys of its own: the index outputs the primary key,
        // not the columns the join keys name, and a key here would be demanded of the index lookup.
        auto indexLookupJoin = MakeIntrusive<TOpIndexLookupJoin>(std::move(indexLookup), join->Pos, joinKind, TJoinIUs{});

        auto mainLookup = MakeIntrusive<TOpTableLookup>(std::move(indexLookupJoin), join->Pos, mainTableCallable,
                                                        read->GetColumns(), std::move(mainLookupKeys), joinKind,
                                                        fetchedRowFilter, std::nullopt, std::move(residualJoinKeys));
        mainLookup->AllowNullKeys = true;
        return MakeIntrusive<TOpIndexLookupJoin>(std::move(mainLookup), join->Pos, joinKind, join->JoinKeys);
    }

    auto lookup = MakeIntrusive<TOpTableLookup>(join->GetLeftInput(), join->Pos, read->GetTable(), read->GetColumns(),
                                                std::move(lookupKeys), joinKind, fetchedRowFilter, prefix, std::move(residualJoinKeys));
    return MakeIntrusive<TOpIndexLookupJoin>(std::move(lookup), join->Pos, joinKind, join->JoinKeys);
}

} // namespace NKqp
} // namespace NKikimr
