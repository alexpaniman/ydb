"""Render the C28 result-boundary DISTINCT amendment without changing production files."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = 'cf3d1399f717ebeef9dd6d2c6280f2a4c9f7effe'
MAIN = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
SOURCE = 'ydb/core/kqp/opt/rbo/kqp_rewrite_select.cpp'


def render_files():
    before = subprocess.check_output(['git', 'show', BASE + ':' + SOURCE], cwd=ROOT.parent, text=True)
    after = subprocess.check_output(['git', 'show', MAIN + ':' + SOURCE], cwd=ROOT.parent, text=True)
    old = """        // Process all aggregations in result item.
        ProcessAggregationsInResultItems(result, aggregationUniqueColNames, expressionsMapPreAgg, groupByKeysExpressionsMap, aggregationTraits,
                                         distinctAggregationTraitsPostAggregate, expressionsMapPostAgg, uniqueAggColumnId, distinctAll, ctx, node->Pos());
"""
    new = """        // Result DISTINCT belongs after both window evaluation and the selected
        // expressions. Preparing its inputs here would project away window keys.
        const bool distinctAfterWindow = distinctAll && !CollectWindowCalls(result).empty();
        ProcessAggregationsInResultItems(result, aggregationUniqueColNames, expressionsMapPreAgg, groupByKeysExpressionsMap, aggregationTraits,
                                         distinctAggregationTraitsPostAggregate, expressionsMapPostAgg, uniqueAggColumnId,
                                         distinctAll && !distinctAfterWindow, ctx, node->Pos());
"""
    assert after.count(old) == 1
    after = after.replace(old, new)
    old = '            Y_ENSURE(distinctAggregationTraitsPostAggregate.AggTraitsList.empty(), "Unsupported grouping sets with distinct all.");'
    new = '            Y_ENSURE(!distinctAll, "Unsupported grouping sets with distinct all.");'
    assert after.count(old) == 1
    after = after.replace(old, new)
    old = '            if (aggregation || distinctAll || hasWindowCall || isDirectGroupRef) {'
    new = '            if (aggregation || (distinctAll && !distinctAfterWindow) || hasWindowCall || isDirectGroupRef) {'
    assert after.count(old) == 1
    after = after.replace(old, new)
    old = """        if (sort) {
            setItemPtr = BuildSort(setItemPtr, sort, groupByKeysExpressionsMap, ctx);
"""
    new = """        if (distinctAfterWindow) {
            TVector<TExprNode::TPtr> distinctTraits;
            TVector<TInfoUnit> distinctKeys;
            for (const auto& column : finalProjection) {
                distinctTraits.push_back(BuildAggregationTraits(column, "distinct", column, ctx, node->Pos()));
                distinctKeys.emplace_back(column);
            }
            setItemPtr = BuildAggregate(setItemPtr, distinctTraits, distinctKeys, /*distinct=*/true, ctx, node->Pos());
        }

        if (sort) {
            setItemPtr = BuildSort(setItemPtr, sort, groupByKeysExpressionsMap, ctx);
"""
    assert after.count(old) == 1
    after = after.replace(old, new)
    return {SOURCE: (before, after)}


def main():
    files = render_files()
    content = ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                      ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                  fromfile='a/' + path, tofile='b/' + path))
                      for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/C28-window-result-projection.patch'
    target.write_text(content)
    with tempfile.TemporaryDirectory(prefix='c28-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env, capture_output=True, check=True)
        checked = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                                 cwd=ROOT.parent, env=env, capture_output=True)
    report = {'id': 'C28', 'base': BASE, 'main_parent': MAIN,
              'patch_sha256': hashlib.sha256(content.encode()).hexdigest(),
              'files': list(files), 'apply_check_passed': checked.returncode == 0,
              'scope': 'Window result DISTINCT is lowered after the final selected projection, without creating early DISTINCT input maps.',
              'compiled': False, 'native_verified': False, 'production_edited': False,
              'preserved': ['Nonwindow DISTINCT uses the existing path.', 'Nondistinct windows use the existing path.',
                            'Ordinary aggregation precedes window evaluation.', 'Final ORDER BY and LIMIT follow result DISTINCT.',
                            'Grouping sets with DISTINCT retain their existing explicit unsupported guard.',
                            'Only selected result columns become DISTINCT keys; hidden window and grouping columns do not.'],
              'limits': ['Independent C12/C13/F01 fixes are not included.',
                         'DISTINCT ordering by an unselected expression retains existing supported-scope constraints.'],
              'pending': ['Independent source review, fixed compile and native semantic controls.']}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': checked.returncode == 0}))
    assert checked.returncode == 0, 'Apply-check failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
