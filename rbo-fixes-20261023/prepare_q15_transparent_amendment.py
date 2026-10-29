"""Create a base-relative wrapper-reuse proposal without checkout edits."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parent
    case = json.loads((root / 'cases' / 'Q15.json').read_text())
    path = 'ydb/core/kqp/opt/rbo/rules/assign_stages.cpp'
    read = lambda revision: subprocess.check_output(['git', 'show', revision + ':' + path], cwd=root.parent, text=True)
    source = 'c117f50129b649140196ce70a29c9b5f39f780b8'
    original, text = read(case['base']), read(source)
    anchor = 'void AssignStage(IOperator* input, TRBOContext& ctx, TPlanProps& props) {'
    helper = '''bool CanReuseWindowPartition(const TOpWindow& window) {
    auto partitionKeys = window.GetPartitionKeys();
    auto* input = window.GetInput().Get();
    const auto stage = input->Props.StageId;
    if (!stage || partitionKeys.Items().empty()) {
        return false;
    }
    while (input->Kind == EOperator::Map || input->Kind == EOperator::Filter) {
        const auto* child = input->GetChild(0).Get();
        if (child->Props.StageId != stage) {
            return false;
        }
        if (input->Kind == EOperator::Map) {
            const auto& map = CastOperator<TOpMap>(*input);
            for (size_t i = 0; i < partitionKeys.Items().size(); ++i) {
                if (const auto* expression = map.GetMapElements().Find(partitionKeys.Items()[i])) {
                    if (!expression->IsColumnAccess()) {
                        return false;
                    }
                    partitionKeys.ReplaceAt(i, expression->GetColumnAccess());
                }
            }
        }
        input = input->GetChild(0).Get();
    }
    // The preceding window established the partition using these exact ordered
    // keys. Same-stage maps/filters preserve task assignment; direct aliases
    // only rename its keys. No logical distribution metadata is consulted.
    return input->Kind == EOperator::Window && input->Props.StageId == stage
        && CastOperator<TOpWindow>(*input).GetPartitionKeys().Items() == partitionKeys.Items();
}

'''
    if text.count(anchor) != 1:
        raise ValueError('Assign-stage source changed')
    text = text.replace(anchor, helper + anchor)
    before = '''        const bool sameWindowPartition = enableShuffleElimination && !window->GetPartitionKeys().Items().empty()
            && window->GetInput()->Kind == EOperator::Window
            && CastOperator<TOpWindow>(*window->GetInput()).GetPartitionKeys().Items() == window->GetPartitionKeys().Items();'''
    after = '''        const bool sameWindowPartition = enableShuffleElimination && CanReuseWindowPartition(*window);'''
    if text.count(before) != 1:
        raise ValueError('Q15 source shape changed')
    text = text.replace(before, after).replace('The immediately preceding window already established this exact',
                                              'The preceding window already established this exact')
    patch = ''.join(difflib.unified_diff(original.splitlines(True), text.splitlines(True),
                                       fromfile='a/' + path, tofile='b/' + path))
    folder = root / 'proposed-quality-amendments'
    folder.mkdir(exist_ok=True)
    target = folder / 'Q15-transparent-wrappers.patch'
    target.write_text(patch)
    result = subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(target)],
                            cwd=root / 'worktrees' / 'verify', capture_output=True)
    metadata = {'id': 'Q15', 'base': case['base'], 'source_branch': source,
        'status': 'amendment_proposed_not_applied', 'files': [path],
        'scope': 'Effective shuffle-elimination gate plus prior Window proof through same-stage Map/Filter, including direct aliases of partition keys.',
        'remaining_open': ['General reuse from joins/aggregates or arbitrary stage graph inputs', 'Partition-key subset proofs', 'Replicate and intervening exchange proofs'],
        'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(), 'apply_check_passed': result.returncode == 0,
        'diagnostic_sha256': hashlib.sha256(result.stdout + result.stderr).hexdigest(),
        'compiled': False, 'native_verified': False, 'production_edited': False,
        'proof': ['Map and Filter retain the exact input StageId when no Replicate/source stage boundary is crossed.',
                  'Direct row-member aliases preserve type/value/hash and are substituted in positional key order.',
                  'Unknown/computed keys, other operators and any StageId change terminate the proof.',
                  'The preceding Window always established its own nonempty partition; C37 metadata is not used.'],
        'validation_required': ['Four-node semantic and shuffle-count tests with retained filters, direct aliases and computed payload maps.',
            'Computed/nested-field partition keys, different key order and subset keys retain shuffles.',
            'Sort, Limit, Replicate and exchange boundaries retain the previous conservative behavior.',
            'Config default and explicit on/off overrides keep their existing semantics.']}
    (folder / 'Q15-transparent-wrappers.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps({'id': 'Q15', 'amendment_apply_check_passed': result.returncode == 0,
                      'patch_sha256': metadata['patch_sha256']}))


if __name__ == '__main__':
    main()
