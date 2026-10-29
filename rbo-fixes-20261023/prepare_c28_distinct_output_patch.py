"""Restore C28's final projection boundary without touching production files."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = '18e12ec833db741848ac2bcf5a88b80f4cfdb6d3'
SOURCE = 'ydb/core/kqp/opt/rbo/kqp_rewrite_select.cpp'


def render_files():
    before = subprocess.check_output(['git', 'show', BASE + ':' + SOURCE], cwd=ROOT.parent, text=True)
    old = '            setItemPtr = BuildAggregate(setItemPtr, distinctTraits, distinctKeys, /*distinct=*/true, ctx, node->Pos());\n'
    new = old + '''            // Keep the selected-result boundary after DISTINCT as on the
            // ordinary path: its keys and result traits share column names.
            setItemPtr = BuildAggregateExpressionMap(setItemPtr, {},
                BuildExpressionsFromColumns(distinctKeys, ctx, node->Pos()), ctx, node->Pos());
'''
    assert before.count(old) == 1
    return {SOURCE: (before, before.replace(old, new))}


def main():
    files = render_files()
    patch = ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                    ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + path, tofile='b/' + path))
                    for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/C28-distinct-output-projection.patch'
    target.write_text(patch)
    with tempfile.TemporaryDirectory(prefix='c28-output-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env, capture_output=True, check=True)
        checked = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                                 cwd=ROOT.parent, env=env, capture_output=True)
    report = {'finding': 'C28', 'base': BASE, 'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
              'files': list(files), 'apply_check_passed': checked.returncode == 0,
              'source_diagnosis': 'Aggregate annotation exposes both key and same-named DISTINCT result fields. A projecting result Map provides the unique selected row schema, as the ordinary DISTINCT and ordered result paths already do.',
              'contracts': ['The extra map is projecting by the existing helper default.',
                            'Each final distinct key becomes one identity definition; no scalar/window expression is reevaluated.',
                            'Importer resolves each identity against the original aggregate binding and assigns a fresh output IU.',
                            'Distinct traits and keys have identical value types, so member selection among their duplicated annotation names is type equivalent.',
                            'Both ordered and unordered outputs now retain the normal projection boundary.',
                            'Existing nonwindow and nondistinct paths are unchanged.'],
              'compiled': False, 'native_executed': False, 'production_sources_edited': False,
              'generated_inputs_saved': False}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': checked.returncode == 0}))
    assert checked.returncode == 0, 'Apply check failed; diagnostics suppressed'


if __name__ == '__main__':
    main()
