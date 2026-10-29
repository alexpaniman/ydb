"""Prepare the local C31 copy-stability guard without mutating a checkout."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = 'add2cf0b3d838963596676c952622e6995b9abf6'
SOURCE = 'ydb/core/kqp/opt/rbo/rules/inline_join_filters.cpp'

HELPER = r'''
// The general copy policy permits reductions whose result can depend on input
// order. Reconstructing a left row requires the copied producer's values to
// agree, including values hidden behind later casts or referenced subplans.
bool HasStableJoinFilterReductions(const IOperator& root, const TSubplans& subplans, TExprContext& ctx) {
    THashSet<const IOperator*> visited;
    TVector<const IOperator*> pending{&root};
    while (!pending.empty()) {
        const auto* op = pending.back();
        pending.pop_back();
        if (!visited.insert(op).second) {
            continue;
        }
        if (op->Kind == EOperator::Aggregate) {
            const auto& aggregate = CastOperator<TOpAggregate>(*op);
            const auto* input = aggregate.GetInput().Get();
            if (!input->Type) {
                return false;
            }
            for (const auto key : aggregate.GetKeyColumns().Items()) {
                // A grouping key can expose an order-dependent representative
                // even when every reduction is COUNT.
                if (!HasExactScalarIdentity(input->GetIUType(key, ctx))) {
                    return false;
                }
            }
            for (const auto& [output, traits] : aggregate.GetAggregationTraits().Items()) {
                Y_UNUSED(output);
                if (traits.AggFunction == "count") {
                    continue;
                }
                const auto* type = input->GetIUType(traits.Input, ctx);
                if (!type) {
                    return false;
                }
                if (traits.AggFunction == "sum") {
                    const auto& valueType = RemoveOptionality(*type);
                    if (valueType.GetKind() != ETypeAnnotationKind::Data
                        || !IsDataTypeIntegral(valueType.Cast<TDataExprType>()->GetSlot())) {
                        return false;
                    }
                } else if ((traits.AggFunction != "min" && traits.AggFunction != "max" && traits.AggFunction != "distinct")
                    || !HasExactScalarIdentity(type)) {
                    return false;
                }
            }
        }
        for (const auto* child : op->GetChildren()) {
            pending.push_back(child);
        }
        for (const auto call : op->GetSubplanIUs(subplans)) {
            pending.push_back(subplans.At(call).Plan.Get());
        }
    }
    return true;
}
'''


def render_files():
    before = subprocess.check_output(['git', 'show', BASE + ':' + SOURCE], cwd=ROOT.parent, text=True)
    anchor = '\n}\n\nnamespace NKikimr {\nnamespace NKqp {\n'
    assert before.count(anchor) == 1
    after = before.replace(anchor, HELPER + anchor)
    anchor = '    auto leftInput = join->GetLeftInput();\n'
    replacement = anchor + '''    // Keep the same sharing decision as the optimizer and physical compiler.
    const bool allowChannelSpilling = ctx.KqpCtx.Config->GetEnableQueryServiceSpilling()
        && (ctx.KqpCtx.IsGenericQuery() || ctx.KqpCtx.IsScanQuery()) && ctx.KqpCtx.Config->SpillingEnabled();
    if (!allowChannelSpilling) {
        Y_ENSURE(HasStableJoinFilterReductions(*leftInput, props.Subplans, ctx.ExprCtx),
            "Join filter reconstruction requires stable copied values");
    }
'''
    assert after.count(anchor) == 1
    after = after.replace(anchor, replacement)
    anchor = '''            Y_ENSURE(CanDuplicateSubtree(*props.Subplans.At(call).Plan, props.Subplans),
                "Join filter domain requires repeatable subplans");
'''
    replacement = anchor + '''            Y_ENSURE(HasStableJoinFilterReductions(*props.Subplans.At(call).Plan, props.Subplans, ctx.ExprCtx),
                "Join filter domain requires stable subplan values");
'''
    assert after.count(anchor) == 1
    after = after.replace(anchor, replacement)
    return {SOURCE: (before, after)}


def main():
    files = render_files()
    patch = ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                    ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + path, tofile='b/' + path))
                    for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/C31-copy-stability.patch'
    target.write_text(patch)
    with tempfile.TemporaryDirectory(prefix='c31-stability-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env, capture_output=True, check=True)
        check = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                               cwd=ROOT.parent, env=env, capture_output=True)
    report = {'id': 'C31', 'base': BASE, 'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
              'apply_check_passed': check.returncode == 0, 'files': list(files),
              'scope': 'Local safety gate for C31-created copied producers and deduplicated called-subplan evaluation; global copy policy unchanged.',
              'compiled': False, 'native_verified': False, 'production_edited': False,
              'pending': ['Independent source review.', 'Direct-IR and native safety/positive controls.',
                          'Broader identities and volatile occurrence semantics remain outstanding.']}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': check.returncode == 0}))
    assert check.returncode == 0, 'Apply check failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
