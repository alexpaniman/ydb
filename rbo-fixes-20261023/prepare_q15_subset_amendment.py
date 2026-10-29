"""Prepare a base-relative Q15 subset contract without checkout mutation."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
PREFIX = 'ydb/core/kqp/opt/rbo/'


def read(path):
    return subprocess.check_output(['git', 'show', BASE + ':' + PREFIX + path], cwd=ROOT.parent, text=True)


def replace(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Pinned source shape differs; review the amendment')
    return text.replace(before, after)


def main():
    names = ('kqp_rbo_utils.h', 'kqp_rbo_utils.cpp', 'kqp_operator.h',
             'kqp_rbo_compute_statistics.cpp', 'rules/assign_stages.cpp')
    originals = {name: read(name) for name in names}
    edited = dict(originals)
    edited['kqp_rbo_utils.h'] = replace(edited['kqp_rbo_utils.h'], 'class TOpAggregate;',
                                      'class TOpAggregate;\nclass TOpWindow;')
    edited['kqp_rbo_utils.h'] = replace(edited['kqp_rbo_utils.h'],
        'bool CanEliminateAggregateShuffle(const TOpAggregate& aggregate, const TRBOContext& ctx);',
        '''bool CanEliminateAggregateShuffle(const TOpAggregate& aggregate, const TRBOContext& ctx);
// The actual ordered hash key, translated to window partition IUs, or no proof.
TOrderedIUs<> GetWindowPreservedShuffling(const TOpWindow& window, const TRBOContext& ctx);''')
    helper = '''TOrderedIUs<> GetWindowPreservedShuffling(const TOpWindow& window, const TRBOContext& ctx) {
    const bool enabled = ctx.KqpCtx.Config->OptShuffleElimination.Get()
        .GetOrElse(ctx.KqpCtx.Config->GetDefaultEnableShuffleElimination());
    if (!enabled || window.GetPartitionKeys().Items().empty()) {
        return {};
    }

    const auto& keys = window.GetPartitionKeys().Items();
    TVector<std::optional<TInfoUnitId>> translated;
    translated.reserve(keys.size());
    for (const auto key : keys) {
        translated.emplace_back(key);
    }
    const auto* input = window.GetInput().Get();
    while (input->Kind == EOperator::Map || input->Kind == EOperator::Filter) {
        const auto* child = input->GetChild(0).Get();
        if (input->Props.StageId && child->Props.StageId && input->Props.StageId != child->Props.StageId) {
            return {};
        }
        if (input->Kind == EOperator::Map) {
            const auto& map = CastOperator<TOpMap>(*input);
            for (auto& key : translated) {
                if (!key) {
                    continue;
                }
                if (const auto* element = map.GetMapElements().Find(*key)) {
                    if (element->IsColumnAccess()) {
                        key = element->GetColumnAccess();
                    } else {
                        key.reset();
                    }
                }
            }
        }
        input = child;
    }

    // Only these producers supply a local proof: Window publishes its actual
    // chosen partition below, and a column read resolves storage hash keys.
    // In particular, never inherit an unproved join/aggregate distribution.
    const bool columnSource = input->Kind == EOperator::Source
        && CastOperator<TOpRead>(*input).GetTableStorageType() == NYql::EStorageType::ColumnStorage;
    if ((input->Kind != EOperator::Window && !columnSource) || !input->Props.Metadata) {
        return {};
    }
    const auto& shuffledBy = input->Props.Metadata->ShuffledByColumns;
    if (shuffledBy.Items().empty() || !shuffledBy.Unordered().IsSubsetOf(input->GetOutputIUs())) {
        return {};
    }

    TOrderedIUs<> preserved;
    for (const auto key : shuffledBy.Items()) {
        bool found = false;
        for (size_t i = 0; i < translated.size(); ++i) {
            if (translated[i] && *translated[i] == key) {
                preserved.Append(keys[i]);
                found = true;
                break;
            }
        }
        if (!found) {
            return {};
        }
    }
    // Extra partition keys are harmless, but retain the input's hash order,
    // multiplicity and types rather than claiming a hash of the full key set.
    return preserved;
}

'''
    edited['kqp_rbo_utils.cpp'] = replace(edited['kqp_rbo_utils.cpp'],
        'bool SortMatchesKeyOrder(', helper + 'bool SortMatchesKeyOrder(')
    window_start = edited['kqp_operator.h'].index('class TOpWindow:')
    window_end = edited['kqp_operator.h'].index('\nclass TOpFilter:', window_start)
    window_class = edited['kqp_operator.h'][window_start:window_end]
    window_class = replace(window_class, '    void PropagateLiveness(ILivenessContext& ctx) override;',
        '''    void PropagateLiveness(ILivenessContext& ctx) override;
    void ComputeMetadata(TRBOContext& ctx, TPlanProps& planProps) override;''')
    edited['kqp_operator.h'] = edited['kqp_operator.h'][:window_start] + window_class + edited['kqp_operator.h'][window_end:]
    metadata = '''void TOpWindow::ComputeMetadata(TRBOContext& ctx, TPlanProps& planProps) {
    IUnaryOperator::ComputeMetadata(ctx, planProps);
    if (!Props.Metadata) {
        return;
    }

    auto& shuffledBy = Props.Metadata->ShuffledByColumns;
    if (!Props.StageId) {
        auto preserved = GetWindowPreservedShuffling(*this, ctx);
        shuffledBy = preserved.Items().empty() ? GetPartitionKeys() : std::move(preserved);
        return;
    }

    // Physical rewrites recompute metadata after assignment. Describe the
    // existing connection, never make a new decision from changed input facts.
    shuffledBy.Clear();
    if (!GetInput()->Props.StageId) {
        return;
    }
    const auto& graph = planProps.StageGraph;
    const auto edges = graph.Connections.find({*GetInput()->Props.StageId, *Props.StageId});
    if (edges == graph.Connections.end() || edges->second.size() != 1) {
        return;
    }
    const auto& connection = edges->second.front();
    if (const auto* shuffle = dynamic_cast<const TShuffleConnection*>(connection.Get())) {
        shuffledBy = shuffle->Keys;
    } else if (IsConnection<TMapConnection>(connection)) {
        shuffledBy = GetWindowPreservedShuffling(*this, ctx);
    }
}

'''
    edited['kqp_rbo_compute_statistics.cpp'] = replace(edited['kqp_rbo_compute_statistics.cpp'],
        '/**\n * Default statistics and cost computation for unary operators\n */',
        metadata + '/**\n * Default statistics and cost computation for unary operators\n */')
    before = '''        const auto newStageId = props.StageGraph.AddStage();
        window->Props.StageId = newStageId;
        if (!window->GetPartitionKeys().Items().empty()) {'''
    after = '''        const auto preserved = GetWindowPreservedShuffling(*window, ctx);
        const auto& expected = preserved.Items().empty() ? window->GetPartitionKeys() : preserved;
        Y_ENSURE(!window->Props.Metadata || window->Props.Metadata->ShuffledByColumns.Items() == expected.Items(),
            "Window partition proof changed after metadata computation");

        const auto newStageId = props.StageGraph.AddStage();
        window->Props.StageId = newStageId;
        if (!preserved.Items().empty()) {
            props.StageGraph.Connect(inputStageId, newStageId, MakeIntrusive<TMapConnection>(outputIndex));
        } else if (!window->GetPartitionKeys().Items().empty()) {'''
    edited['rules/assign_stages.cpp'] = replace(edited['rules/assign_stages.cpp'], before, after)
    patch = ''.join(''.join(difflib.unified_diff(originals[name].splitlines(True), edited[name].splitlines(True),
        fromfile='a/' + PREFIX + name, tofile='b/' + PREFIX + name)) for name in names)
    target = ROOT / 'proposed-quality-amendments' / 'Q15-subset-contract.patch'
    target.write_text(patch)
    check = subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(target)],
                           cwd=ROOT / 'worktrees' / 'verify', capture_output=True)
    evidence = {'id': 'Q15', 'base': BASE, 'status': 'amendment_proposed_not_applied',
        'files': [PREFIX + name for name in names], 'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
        'apply_check_passed': check.returncode == 0,
        'diagnostic_sha256': hashlib.sha256(check.stdout + check.stderr).hexdigest(),
        'scope': 'Shared metadata/stage partition-subset proof for prior Windows and column sources through direct Map aliases and Filter.',
        'source_review_pending': True, 'compiled': False, 'native_verified': False, 'production_edited': False,
        'remaining_limits': ['Join and Aggregate producers are conservatively unproved.',
            'Replicate, Sort, Limit, streaming lookup and union distributions are conservatively unproved.',
            'Only direct row-member aliases preserve key type/value; computed keys may supplement but cannot replace required hash keys.',
            'Metadata override overlaps C37 and must use this shared final body when combining branches.']}
    target.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'patch_sha256': evidence['patch_sha256'], 'apply_check_passed': evidence['apply_check_passed']}))
    if check.returncode:
        raise SystemExit('Patch validation failed; diagnostic payload suppressed')


if __name__ == '__main__':
    main()
