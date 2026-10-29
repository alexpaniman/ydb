"""Prepare a bounded-region RBO worklist as a base-relative artifact only."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
PREFIX = 'ydb/core/kqp/opt/rbo/'


def replace(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Pinned production source anchor changed')
    return text.replace(before, after, 1)


CONTRACT = '''
    // Opt in only when matching reads this subtree and at most OutputIUs.
    // A successful rewrite preserves the root output IDs and may mutate/create
    // nodes only within this many child edges of the replacement root. It must
    // not mutate other nodes, ownership outside this region, subplan bindings,
    // the IU registry, stages or context-global state. It must preserve the set
    // of referenced subplans and create no new sharing. Do not rewrite through
    // a Replicate port or change a shared producer's output IDs inside the
    // region: other consumers would need their own invalidation. Existing nodes beyond
    // the region remain unchanged, including their output caches. A failed
    // rewrite must have no effect. Unknown rules use the conservative loop.
    virtual std::optional<ui32> LocalRewriteDepth() const { return std::nullopt; }
'''


WORKLIST = '''    bool useLocalWorklist = !Rules.empty();
    for (const auto& rule : Rules) {
        useLocalWorklist &= rule->LocalRewriteDepth().has_value()
            && (rule->Props & ~ui32(ERuleProperties::RequireOutputIUs)) == 0;
    }
    if (useLocalWorklist) {
        struct TOwnedEntry {
            TIntrusivePtr<IOperator> Current;
            TIntrusivePtr<IOperator> Parent;
            size_t ChildIndex;
            std::optional<TInfoUnitId> SubplanIU;
        };
        TVector<TOwnedEntry> initial;
        THashMap<IOperator*, const TIntrusivePtr<IOperator>*> owners;
        const auto recordOwner = [&](TIntrusivePtr<IOperator>& slot) {
            const auto [it, inserted] = owners.emplace(slot.Get(), &slot);
            useLocalWorklist &= inserted || it->second == &slot;
        };
        recordOwner(root.MutableChild(0));
        for (const auto& item : root) {
            initial.push_back({TIntrusivePtr<IOperator>(item.Current),
                TIntrusivePtr<IOperator>(item.Parent), item.ChildIndex, item.SubplanIU});
            // Replicate ports forward one canonical producer slot. Distinct
            // owning slots for one node need general consumer invalidation.
            useLocalWorklist &= item.Current->Kind != EOperator::CBOTree && !item.Current->Props.StageId;
            for (size_t child = 0; child < item.Current->GetChildCount(); ++child) {
                recordOwner(item.Current->MutableChild(child));
            }
            for (const auto iu : item.Current->GetSubplanIUs(root.PlanProps.Subplans)) {
                if (root.PlanProps.Subplans.Find(iu)) {
                    recordOwner(root.PlanProps.Subplans.MutablePlan(iu));
                }
            }
        }
        if (useLocalWorklist) {
            EnsureRequiredProps(root, ERuleProperties::RequireOutputIUs, computedProps, ctx, StageName);
            TVector<TOwnedEntry> work(initial.rbegin(), initial.rend());
            while (!work.empty()) {
                auto entry = std::move(work.back());
                work.pop_back();
                auto& op = entry.Parent ? entry.Parent->MutableChild(entry.ChildIndex)
                    : entry.SubplanIU ? root.PlanProps.Subplans.MutablePlan(*entry.SubplanIU)
                    : root.MutableChild(0);
                if (op.Get() != entry.Current.Get()) {
                    // A contract not yet admitted by this scheduler changed a
                    // queued owner. Resume from a fresh conservative traversal.
                    break;
                }
                for (const auto& rule : Rules) {
                    if (!rule->QuickMatch(op, root.PlanProps)) {
                        continue;
                    }
                    const auto previousOutputs = op->GetOutputIUs();
                    TRuleTraceAttempt traceAttempt(ctx, rule->RuleName);
                    const bool applied = rule->MatchAndApply(op, ctx, root.PlanProps);
                    Y_ENSURE(op, "Rule left a null plan edge");
                    traceAttempt.CloseRule();
                    if (!applied) {
                        traceAttempt.SubmitIfHasInfo(root, StageName);
                        continue;
                    }

                    TVector<TOwnedEntry> region;
                    const auto collect = [&](const auto& self, TIntrusivePtr<IOperator> current,
                        TIntrusivePtr<IOperator> parent, size_t childIndex,
                        std::optional<TInfoUnitId> subplanIU, ui32 depth) -> void {
                        if (depth) {
                            for (size_t child = 0; child < current->GetChildCount(); ++child) {
                                self(self, current->GetChild(child), current, child, subplanIU, depth - 1);
                            }
                        }
                        region.push_back({std::move(current), std::move(parent), childIndex, subplanIU});
                    };
                    collect(collect, op, entry.Parent, entry.ChildIndex, entry.SubplanIU, *rule->LocalRewriteDepth());
                    // Child caches outside the declared region are unchanged.
                    // Refresh inside it in postorder; the equal root output
                    // makes every ancestor's output cache remain valid.
                    for (const auto& changed : region) {
                        changed.Current->RecomputeLocalOutputIUs();
                    }
                    Y_ENSURE(op->GetOutputIUs() == previousOutputs, "Local rewrite changed its root output IUs");
                    computedProps &= ERuleProperties::RequireOutputIUs;
                    // New/mutated descendants may expose new candidates. Visit
                    // them before the replacement and before queued ancestors.
                    work.insert(work.end(), region.rbegin(), region.rend());

                    YQL_CLOG(TRACE, CoreDq) << "Applied rule:" << rule->RuleName;
                    if (needToLog && rule->LogRule) {
                        YQL_CLOG(TRACE, CoreDq) << "Plan after applying rule:\\n" << root.PlanToString(ctx.ExprCtx);
                    }
                    traceAttempt.SubmitApplied(root, StageName);
                    ++numMatches;
                    Y_ENSURE(numMatches < maxNumOfMatches);
                    break;
                }
            }
            // Keep one fresh correctness pass, including property recomputation
            // on demand. Unknown rule effects always use that loop directly.
            computedProps = 0;
        }
    }

'''


def render_files():
    names = ('kqp_rbo.h', 'kqp_operator.h', 'kqp_rbo_rules.h', 'kqp_rbo.cpp')
    before = {name: subprocess.check_output(['git', 'show', BASE + ':' + PREFIX + name],
                                           cwd=ROOT.parent, text=True) for name in names}
    after = dict(before)
    after['kqp_rbo.h'] = replace(after['kqp_rbo.h'],
        '    virtual ~IRule() = default;', CONTRACT + '\n    virtual ~IRule() = default;')
    after['kqp_operator.h'] = replace(after['kqp_operator.h'],
        '    virtual const TUnorderedIUs& GetOutputIUs();',
        '''    virtual const TUnorderedIUs& GetOutputIUs();

    // Scheduler-only local refresh: every child output must already be current.
    void RecomputeLocalOutputIUs() {
        Props.OutputIUs.reset();
        ComputeOutputIUs();
    }''')
    after['kqp_rbo_rules.h'] = replace(after['kqp_rbo_rules.h'],
        '    TRewriteRightJoinRule() : ISimplifiedRule("Rewrite right join", ERuleProperties::RequireParents ) {}',
        '''    TRewriteRightJoinRule() : ISimplifiedRule("Rewrite right join", 0) {}
    std::optional<ui32> LocalRewriteDepth() const override { return 0; }''')
    for name, label in [('TPushMapElementsIntoMapRule', 'Push map elements into map'),
                        ('TPushMapElementsThroughInputRule', 'Push map elements through input operator')]:
        anchor = '    ' + name + '() : ISimplifiedRule("' + label + '", ERuleProperties::RequireOutputIUs) {}'
        after['kqp_rbo_rules.h'] = replace(after['kqp_rbo_rules.h'], anchor,
            anchor + '\n    std::optional<ui32> LocalRewriteDepth() const override { return 2; }')
    after['kqp_rbo.cpp'] = replace(after['kqp_rbo.cpp'],
        '    while (fired && numMatches < maxNumOfMatches) {',
        WORKLIST + '    while (fired && numMatches < maxNumOfMatches) {')
    return {PREFIX + name: (before[name], after[name]) for name in names}


def main():
    files = render_files()
    patch = ''.join(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                    fromfile='a/' + name, tofile='b/' + name)) for name, (before, after) in files.items())
    target = ROOT / 'proposed-quality-amendments' / 'Q28-local-worklist.patch'
    target.write_text(patch)
    check = subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(target)],
                           cwd=ROOT / 'worktrees/verify', capture_output=True)
    evidence = {'id': 'Q28', 'base': BASE, 'status': 'architectural_step_proposed_partial_unverified',
                'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(), 'files': list(files),
                'apply_check_passed': check.returncode == 0,
                'diagnostic_sha256': hashlib.sha256(check.stdout + check.stderr).hexdigest(),
                'compiled': False, 'native_verified': False, 'production_edited': False,
                'scope': 'Explicit bounded-region worklist and local output-IU refresh for right-join and Map-push stages.',
                'remaining_limits': ['All unreviewed rule effects retain full conservative restart/invalidation.',
                                     'No incremental types, metadata, statistics, parents or liveness.',
                                     'Opaque CBO, physical stages and distinct raw owning slots reject the worklist.',
                                     'A fresh final conservative pass remains mandatory.',
                                     'Trace plan serialization costs are unchanged.']}
    target.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'patch_sha256': evidence['patch_sha256'], 'apply_check_passed': check.returncode == 0}))
    if check.returncode:
        raise SystemExit('Patch validation failed; diagnostic payload suppressed')


if __name__ == '__main__':
    main()
