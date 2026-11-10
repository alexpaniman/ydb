#include "kqp_subplan_value.h"
#include "rules/decorrelation/dependent_join_pushdown.h"

namespace NKikimr::NKqp {
namespace {

void AddDomainColumn(TUnorderedIUs& domain, TInfoUnitId iu) {
    if (!domain.Contains(iu)) {
        domain.Add(iu);
    }
}

} // namespace

TIntrusivePtr<IOperator> BuildInExistsValue(const TIntrusivePtr<IOperator>& input,
    TInfoUnitId subplanIU, TPositionHandle pos, TPlanProps& props, bool nativeEqualNulls)
{
    const auto& subplanEntry = props.Subplans.At(subplanIU);
    TIntrusivePtr<IOperator> result;
    auto subplan = CastOperator<IOperator>(subplanEntry.Plan);

    const bool useDependentJoin = HasFreeCorrelation(subplan, subplanEntry.DependentIUs);
    if (subplanEntry.Type == ESubplanType::IN_SUBPLAN || useDependentJoin) {
        TIntrusivePtr<IOperator> leftInput = input;
        auto rightInput = subplan;
        const auto outerIUs = leftInput->GetOutputIUs();
        TVector<TInfoUnitId> originalPlanIUs;
        if (subplanEntry.ResultIU) {
            originalPlanIUs.push_back(*subplanEntry.ResultIU);
        }

        TUnorderedIUs domain;
        if (useDependentJoin) {
            for (const auto& iu : subplanEntry.DependentIUs) {
                AddDomainColumn(domain, iu);
            }
        } else {
            for (const auto& iu : subplanEntry.Tuple.Items()) {
                AddDomainColumn(domain, iu);
            }
        }

        Y_ENSURE(!domain.Empty(), "Cannot decorrelate in/exists subplan without correlated columns");
        for (const auto& iu : domain) {
            Y_ENSURE(outerIUs.Contains(iu),
                     TStringBuilder() << "Correlation column " << props.InfoUnitRegistry.GetDebugName(iu) << " is not produced by the outer plan");
        }

        // For exists we can emulate a mkrk join with 2 values output(true, false).
        bool markMissingAsFalse = subplanEntry.Type == ESubplanType::EXISTS;
        if (!markMissingAsFalse) {
            markMissingAsFalse = true;
            for (size_t i = 0; i < subplanEntry.Tuple.Items().size() && markMissingAsFalse; i++) {
                markMissingAsFalse = !IsNullableIU(leftInput, subplanEntry.Tuple.Items()[i]) && !IsNullableIU(rightInput, originalPlanIUs[i]);
            }
        }

        Y_ENSURE(subplanEntry.Type == ESubplanType::EXISTS || (subplanEntry.Type == ESubplanType::IN_SUBPLAN && subplanEntry.Tuple.Items().size() == 1 && subplanEntry.ResultIU));
        // For in we have to emulate three value result (true, false, null).
        const bool threeValued = !markMissingAsFalse && subplanEntry.Type == ESubplanType::IN_SUBPLAN && subplanEntry.Tuple.Items().size() == 1;

        // The mark join and the domain read leftInput through a Replicate, the domain under fresh IDs.
        auto subplanDomain = MakeSubplanDomain(leftInput, domain, pos, props);
        const TSubstitutions domainColumns(subplanDomain.Keys.Items().begin(), subplanDomain.Keys.Items().end());

        TUnorderedIUs markColumns = subplanDomain.Keys.Right();
        TJoinIUs domainJoinKeys = subplanDomain.Keys;
        TJoinIUs tupleJoinKeys;

        TIntrusivePtr<IOperator> statsSource;
        TUnorderedIUs statsKeys;
        TJoinIUs statsJoinKeys;
        TInfoUnitId compareResultIU = 0;

        TIntrusivePtr<IOperator> matchSource;
        if (useDependentJoin) {
            matchSource = std::move(subplanDomain).Bind(rightInput, pos);

            for (size_t i = 0; i < subplanEntry.Tuple.Items().size(); i++) {
                AddDomainColumn(markColumns, originalPlanIUs[i]);
                tupleJoinKeys.Add(subplanEntry.Tuple.Items()[i], originalPlanIUs[i]);
            }

            if (threeValued) {
                compareResultIU = originalPlanIUs[0];
            }
        } else {
            // The match join and the statistics read the subplan through a Replicate.
            if (threeValued) {
                auto hub = TReplicate::Create(rightInput, pos, props.InfoUnitRegistry);
                rightInput = hub->AddOutput();
                statsSource = hub->AddOutput();
            }

            TJoinIUs joinKeys;
            const auto& planIUs = originalPlanIUs;
            for (size_t i = 0; i < subplanEntry.Tuple.Items().size(); i++) {
                joinKeys.Add(domainColumns.At(subplanEntry.Tuple.Items()[i]), planIUs[i]);
            }
            matchSource = MakeIntrusive<TOpJoin>(subplanDomain.Input, rightInput, pos, "Inner", joinKeys);

            if (threeValued) {
                compareResultIU = CastOperator<TOpReplicate>(statsSource)->GetRebindings().At(planIUs[0]);
            }
        }

        TIntrusivePtr<IOperator> matchedDomain = MakeDomainProjection(matchSource, markColumns, pos);
        if (!statsSource && threeValued) {
            // The mark join and the statistics read the matched domain through a Replicate.
            auto hub = TReplicate::Create(matchedDomain, pos, props.InfoUnitRegistry);
            matchedDomain = hub->AddOutput();
            auto stats = hub->AddOutput();
            compareResultIU = stats->GetRebindings().At(compareResultIU);
            for (const auto& key : domainJoinKeys.Items()) {
                statsKeys.Add(stats->GetRebindings().At(key.second));
                statsJoinKeys.Add(key.first, stats->GetRebindings().At(key.second));
            }
            statsSource = stats;
        }

        // Here we want to emulate a mark join, rewriting it into:
        // coalesce(leftjoin(left input, map(true, (dependent join(...)), false).
        // So as result we will get true for columns which survive dependent join and false for rest.
        auto markIU = props.InfoUnitRegistry.AddGenerated("in_mark");
        TMapIUs markElements;
        markElements.Add(markIU, MakeConstant("Bool", "true", pos, &props));
        auto markMap = MakeIntrusive<TOpMap>(matchedDomain, pos, markElements);
        TIntrusivePtr<IOperator> markRight = markMap;
        auto markJoinKeys = useDependentJoin ? MakeNullSafeJoinKeys(leftInput, markRight, domainJoinKeys, pos, nativeEqualNulls, props) : domainJoinKeys;
        for (const auto& key : tupleJoinKeys.Items()) {
            markJoinKeys.Add(key);
        }

        TIntrusivePtr<IOperator> markJoin =
            MakeIntrusive<TOpJoin>(leftInput, markRight, pos, "Left", markJoinKeys);

        TIntrusivePtr<IOperator> resultInput = markJoin;
        auto column = [&](TInfoUnitId iu) { return MakeColumnAccess(iu, *resultInput, pos, &props); };
        auto falseConst = MakeConstant("Bool", "false", pos, &props);
        auto matched = MakeBinaryPredicate("Coalesce", column(markIU), falseConst);

        TMapIUs resultElements;

        if (markMissingAsFalse) {
            resultElements.Add(subplanIU, matched);
        } else if (threeValued) {
            // This one is an attempt to emulate three value result. For projection column we need to know does it contain null or not for each binding. So we have a special
            // pipeline with aggregation lets call it statistics. We will count(1) as num_rows, count(projection column) as num_rows_not_null group by domain columns. 
            // Has null if (num_rows > num_rows_not_null).
            auto rowIU = props.InfoUnitRegistry.AddGenerated("row");
            TMapIUs rowElements;
            rowElements.Add(rowIU, MakeConstant("Uint64", "1", pos, &props));
            auto rowMap = MakeIntrusive<TOpMap>(statsSource, pos, rowElements);

            auto valueCountIU = props.InfoUnitRegistry.AddGenerated("value_count");
            auto rowCountIU = props.InfoUnitRegistry.AddGenerated("row_count");

            // count(projection), count(1)
            TAggregationIUs statsTraits;
            statsTraits.Add(valueCountIU, TOpAggregationTraits{compareResultIU, "count"});
            statsTraits.Add(rowCountIU, TOpAggregationTraits{rowIU, "count"});
            auto statsAggregate = MakeIntrusive<TOpAggregate>(rowMap, statsTraits, TOrderedIUs<>(statsKeys.begin(), statsKeys.end()), EOpPhase::Undefined, /*distinctAll=*/false, pos);

            auto hasNullIU = props.InfoUnitRegistry.AddGenerated("in_has_null");
            auto nonEmptyIU = props.InfoUnitRegistry.AddGenerated("in_non_empty");
            TMapIUs statsElements;
            // Does it have null columns?.
            statsElements.Add(hasNullIU, MakeBinaryPredicate(">", MakeColumnAccess(rowCountIU, NFrontend::TType{NFrontend::ETypeKind::Uint64}, pos, &props),
                MakeColumnAccess(valueCountIU, NFrontend::TType{NFrontend::ETypeKind::Uint64}, pos, &props)));
            if (statsKeys.Empty()) {
                statsElements.Add(nonEmptyIU, MakeBinaryPredicate(">", MakeColumnAccess(rowCountIU, NFrontend::TType{NFrontend::ETypeKind::Uint64}, pos, &props), MakeConstant("Uint64", "0", pos, &props)));
            } else {
                // Always has some rows.
                statsElements.Add(nonEmptyIU, MakeConstant("Bool", "true", pos, &props));
            }
            auto statsMap = MakeIntrusive<TOpMap>(statsAggregate, pos, statsElements);

            TIntrusivePtr<IOperator> statsLeft = markJoin;
            TIntrusivePtr<IOperator> statsRight = statsMap;
            statsJoinKeys = MakeNullSafeJoinKeys(statsLeft, statsRight, statsJoinKeys, pos, nativeEqualNulls, props);

            resultInput = MakeIntrusive<TOpJoin>(statsLeft, statsRight, pos, statsKeys.Empty() ? "Cross" : "Left", statsJoinKeys);

            TVector<TExpression> unknownTerms;
            unknownTerms.push_back(MakeBinaryPredicate("Coalesce", column(hasNullIU), falseConst));

            // This emulates a three value semantis if lookup column is null.
            if (IsNullableIU(leftInput, subplanEntry.Tuple.Items()[0])) {
                auto lookupColumn = column(subplanEntry.Tuple.Items()[0]);
                auto lookupIsNull = MakeNegation(MakeBinaryPredicate("Coalesce", MakeBinaryPredicate("==", lookupColumn, lookupColumn), falseConst));
                unknownTerms.push_back(MakeBinaryPredicate("And", lookupIsNull, MakeBinaryPredicate("Coalesce", column(nonEmptyIU), falseConst)));
            }

            auto unknown = unknownTerms[0];
            for (size_t i = 1; i < unknownTerms.size(); i++) {
                unknown = MakeBinaryPredicate("Or", unknown, unknownTerms[i]);
            }

            auto nullBool = TExpression::FromNative(props.NativeExpressions, props.NativeExpressions->Literal({NFrontend::ETypeKind::Bool, true}, std::monostate{}), &props, pos);
            resultElements.Add(subplanIU, MakeBinaryPredicate("Or", matched, MakeBinaryPredicate("And", unknown, nullBool)));
        } else {
            resultElements.Add(subplanIU, column(markIU));
        }

        result = MakeIntrusive<TOpMap>(resultInput, pos, resultElements);
    }
    // uncorrelated EXISTS
    else {
        auto zero = MakeConstant("Uint64", "0", pos, &props);
        auto limit = MakeIntrusive<TOpLimit>(subplan, pos, MakeConstant("Uint64", "1", pos, &props), EOpPhase::Undefined);

        // The counted rows and their count are distinct IUs.
        auto countInput = props.InfoUnitRegistry.AddGenerated("row");
        auto countResult = props.InfoUnitRegistry.AddGenerated("row_count");
        TMapIUs countMapElements;
        countMapElements.Add(countInput, zero);
        auto countMap = MakeIntrusive<TOpMap>(limit, pos, countMapElements);

        TAggregationIUs aggs;
        aggs.Add(countResult, TOpAggregationTraits{countInput, "count"});
        TOrderedIUs<> keyColumns;

        auto agg = MakeIntrusive<TOpAggregate>(countMap, aggs, keyColumns, EOpPhase::Final, false, pos);

        auto comparePredicate = MakeBinaryPredicate("!=", MakeColumnAccess(countResult, NFrontend::TType{NFrontend::ETypeKind::Uint64}, pos, &props), zero);
        TMapIUs mapElements;
        mapElements.Add(subplanIU, comparePredicate);

        auto map = MakeIntrusive<TOpMap>(agg, pos, mapElements);

        TJoinIUs joinKeys;
        result = MakeIntrusive<TOpJoin>(input, map, pos, "Cross", joinKeys);
    }

    props.Subplans.Remove(subplanIU);

    return result;
}

} // namespace NKikimr::NKqp
