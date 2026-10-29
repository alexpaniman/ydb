"""Extend the base-relative Q07 source patch; never edit production worktrees."""
import difflib
import json
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
base = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
patch = root / 'proposed-quality-patches/Q07.patch'
lines = patch.read_text().splitlines(keepends=True)
original, current = {}, {}
i = 0
while i < len(lines):
    if not lines[i].startswith('--- a/'):
        i += 1
        continue
    path = lines[i][6:].strip()
    assert lines[i + 1].strip() == '+++ b/' + path
    source = subprocess.check_output(['git', 'show', base + ':' + path], text=True)
    original[path] = source
    src = source.splitlines(keepends=True)
    out, cursor = [], 0
    i += 2
    while i < len(lines) and not lines[i].startswith('--- a/'):
        header = re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', lines[i])
        assert header, lines[i]
        start = int(header[1]) - 1
        out.extend(src[cursor:start]); cursor = start
        i += 1
        while i < len(lines) and not lines[i].startswith(('@@ ', '--- a/')):
            item = lines[i]
            if item[0] in ' -':
                assert src[cursor] == item[1:], (path, cursor)
                cursor += 1
            if item[0] in ' +':
                out.append(item[1:])
            i += 1
    out.extend(src[cursor:]); current[path] = ''.join(out)

path = 'ydb/core/kqp/opt/rbo/rules/assign_stages.cpp'
source = subprocess.check_output(['git', 'show', base + ':' + path], text=True)
original[path] = source
source = source.replace('#include <yql/essentials/core/yql_expr_type_annotation.h>\n', '#include <yql/essentials/core/yql_expr_type_annotation.h>\n\n#include <cmath>\n#include <limits>\n')
anchor = 'void FinalizeJoinPhysicalProps(TOpJoin& join, const TRBOContext& rboCtx) {'
helper = '''// A LookupJoin that remains a regular Join was not lowerable after the final
// logical rewrites. Hints can also bypass CBO applicability, so validate here.
bool IsUnloweredLookupJoin(const IOperator& op) {
    return op.Kind == EOperator::Join && (op.Props.JoinAlgo == EJoinAlgoType::LookupJoin
        || op.Props.JoinAlgo == EJoinAlgoType::LookupJoinReverse);
}

bool LookupFallbackNeedsGather(const TOpJoin& join, TExprContext& ctx) {
    for (const auto& [left, right, equalNulls] : join.JoinKeys.Items()) {
        const auto* leftType = join.GetLeftInput()->GetIUType(left, ctx);
        const auto* rightType = join.GetRightInput()->GetIUType(right, ctx);
        Y_ENSURE(leftType && rightType, "No types for lookup fallback keys");
        if (!IsSameAnnotation(*leftType, *rightType)) {
            // Comparison-compatible values need not have identical shuffle hashes.
            return true;
        }
    }
    return false;
}

void SelectLookupFallback(TOpJoin& join, TRBOContext& ctx, TPlanProps& planProps, bool gatherInputs) {
    auto& props = join.Props;
    props.JoinAlgo = EJoinAlgoType::GraceJoin;
    props.UseBlockHashJoin.reset();
    props.Cost.reset();
    const auto joinKind = GetValidJoinKind(join.JoinKind);
    if (joinKind == "Cross") {
        props.JoinAlgo = EJoinAlgoType::MapJoin;
        return;
    }
    const auto& left = join.GetLeftInput();
    const auto& right = join.GetRightInput();
    if (gatherInputs || !left->Props.Metadata || !right->Props.Metadata
        || !left->Props.Statistics || !right->Props.Statistics) {
        return;
    }
    const auto leftStats = BuildOptimizerStatistics(*left, planProps.ColumnLineage, true, ctx.TypeCtx);
    const auto rightStats = BuildOptimizerStatistics(*right, planProps.ColumnLineage, true, ctx.TypeCtx);
    auto leftNode = std::make_shared<TRelOptimizerNode>("lookup_fallback_left", leftStats);
    auto rightNode = std::make_shared<TRelOptimizerNode>("lookup_fallback_right", rightStats);
    TVector<TJoinColumn> leftKeys;
    TVector<TJoinColumn> rightKeys;
    for (const auto& [leftKey, rightKey, equalNulls] : join.JoinKeys.Items()) {
        const auto* leftSource = planProps.ColumnLineage.Find(leftKey);
        const auto* rightSource = planProps.ColumnLineage.Find(rightKey);
        leftKeys.emplace_back(leftSource ? leftSource->GetRawAlias() : TString{}, ToString(leftKey));
        rightKeys.emplace_back(rightSource ? rightSource->GetRawAlias() : TString{}, ToString(rightKey));
        leftKeys.back().EqualNulls = rightKeys.back().EqualNulls = equalNulls;
    }
    const auto kind = ConvertToJoinKind(joinKind);
    const bool supportsBlock = ctx.KqpCtx.Config->GetUseBlockHashJoin()
        && (joinKind == "Inner" || joinKind == "Left" || joinKind == "LeftSemi" || joinKind == "LeftOnly");
    double bestCost = std::numeric_limits<double>::infinity();
    for (const auto algorithm : {EJoinAlgoType::GraceJoin, EJoinAlgoType::MapJoin, EJoinAlgoType::ReverseBlockJoin}) {
        // The row Map implementation cannot preserve a right/full outer side.
        if (algorithm == EJoinAlgoType::MapJoin
            && ((joinKind != "Inner" && joinKind != "Left" && joinKind != "LeftSemi" && joinKind != "LeftOnly")
                || (!join.JoinFilters.empty() && !supportsBlock))) {
            continue;
        }
        if (algorithm == EJoinAlgoType::ReverseBlockJoin && !supportsBlock) {
            continue;
        }
        if (!ctx.CBOCtx.IsJoinApplicable(leftNode, rightNode, leftKeys, rightKeys, algorithm, kind)) {
            continue;
        }
        // A failed lookup changes distribution. Cost hash candidates with fresh
        // shuffles; Map retains the provider's small-right-side applicability cap.
        const bool shuffle = algorithm != EJoinAlgoType::MapJoin;
        const auto stats = ctx.CBOCtx.ComputeJoinStatsV2(leftStats, rightStats, leftKeys, rightKeys,
            algorithm, kind, nullptr, shuffle, shuffle, nullptr);
        if (std::isfinite(stats.Cost) && stats.Cost < bestCost) {
            bestCost = stats.Cost;
            props.JoinAlgo = algorithm;
            props.Cost = stats.Cost;
        }
    }
}

'''
assert anchor in source
source = source.replace(anchor, helper + anchor, 1)
source = source.replace('void AssignStage(IOperator* input, TRBOContext& ctx, TPlanProps& props) {',
    'void AssignStage(IOperator* input, TRBOContext& ctx, TPlanProps& props, bool gatherLookupInputs) {', 1)
anchor = '''        if (join->JoinKind == "Cross" || join->Props.JoinAlgo == EJoinAlgoType::MapJoin) {'''
replacement = '''        if (gatherLookupInputs) {
            // Casts run inside the join. Gather first when equality does not prove
            // that the two physical key representations have the same hash.
            props.StageGraph.Connect(leftStage, newStageId, MakeIntrusive<TUnionAllConnection>(leftOutputIndex));
            props.StageGraph.Connect(rightStage, newStageId, MakeIntrusive<TUnionAllConnection>(rightOutputIndex));
        } else if (join->JoinKind == "Cross" || join->Props.JoinAlgo == EJoinAlgoType::MapJoin) {'''
assert anchor in source
source = source.replace(anchor, replacement, 1)
anchor = '''void TAssignStagesStage::RunStage(TOpRoot& root, TRBOContext& ctx) {
    for (const auto& item : root) {
        AssignStage(item.Current, ctx, root.PlanProps);
    }
}'''
replacement = '''void TAssignStagesStage::RunStage(TOpRoot& root, TRBOContext& ctx) {
    bool hasLookupFallback = false;
    for (const auto& item : root) {
        hasLookupFallback |= IsUnloweredLookupJoin(*item.Current);
    }
    THashSet<IOperator*> gatheredLookups;
    if (hasLookupFallback) {
        root.ComputePlanStatistics(ctx);
        for (const auto& item : root) {
            if (IsUnloweredLookupJoin(*item.Current)) {
                auto& join = CastOperator<TOpJoin>(*item.Current);
                const bool gather = LookupFallbackNeedsGather(join, ctx.ExprCtx);
                SelectLookupFallback(join, ctx, root.PlanProps, gather);
                if (gather) {
                    gatheredLookups.insert(item.Current);
                }
            }
        }
        // CBO's saved edge decisions may also describe ancestors of the changed
        // join. Discard them for this root before any connection is constructed.
        for (const auto& item : root) {
            item.Current->Props.LeftShuffleBy.reset();
            item.Current->Props.RightShuffleBy.reset();
            if (item.Current->Props.Metadata) {
                item.Current->Props.Metadata->ShuffledByColumns.Clear();
            }
        }
    }
    for (const auto& item : root) {
        AssignStage(item.Current, ctx, root.PlanProps, gatheredLookups.contains(item.Current));
    }
}'''
assert anchor in source
source = source.replace(anchor, replacement, 1)
current[path] = source
patch.write_text(''.join(''.join(difflib.unified_diff(original[p].splitlines(keepends=True),current[p].splitlines(keepends=True),fromfile='a/'+p,tofile='b/'+p)) for p in current))
metadata_path=root/'proposed-quality-patches/Q07.json'
metadata=json.loads(metadata_path.read_text())
metadata['status']='proposed_patch_only'
metadata['files']=list(current)
metadata['scope']='Shared typed lookup eligibility and target/key preparation for costing and lowering; final stage-assignment validation recosts any unlowered forced or invalidated lookup and clears stale distribution decisions.'
metadata['validation_plan'].extend([
 'Compare eligible lookup plans unchanged, ordinary rejected candidates never costed as lookups, forced/late-rejected candidates use supported physical algorithms.',
 'Large right fallback never bypasses CBO Map size cap; missing statistics choose Grace; mixed key annotation fallback gathers both inputs before comparison.',
 'Nested and shared consumers discard ancestor CBO shuffle claims after fallback; all supported regular join kinds preserve rows.',
 'Q29 integration keeps one BuildLookupJoinFetchedRowFilter declaration and uses the Q29 safe comparison deduplication body from Q07 PrepareLookupJoin.'
])
metadata['limits']=[
 'No compilation, native execution or plan-quality measurement yet.',
 'Fallback recosts the selected join orientation locally; it does not rerun global join-order search after a forced hint or later logical rewrite.',
 'C50 integration should merge its general gather proof with this lookup-fallback-only gather handling.',
 'Q29 overlaps the shared fetched-row filter helper and removes the same obsolete duplicated helpers; integrate explicitly, retaining one helper.',
 'Integration still requires C47 imported-read settings policy and the separate index-family policy corrections.'
]
metadata['static_checks']=['Initial shared-preparation patch passed baseline apply-check; extended fallback patch awaits fresh apply-check and independent source review.']
metadata_path.write_text(json.dumps(metadata,indent=2)+'\n')
print(json.dumps({'files':len(current),'patch_bytes':patch.stat().st_size,'status':metadata['status']}))
