"""Prepare a standalone pinned-main F01 producer repair; no checkout mutation."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
SOURCE = 'ydb/core/kqp/opt/rbo/kqp_rewrite_select.cpp'


def render_files():
    before = subprocess.check_output(['git', 'show', BASE + ':' + SOURCE], cwd=ROOT.parent, text=True)
    anchor = '''                originalColName = TInfoUnit(member.Name().StringValue());
                expressionsMapPreAgg.emplace_back(originalColName.GetFullName(), lambda.Ptr(), false);
'''
    replacement = '''                originalColName = TInfoUnit(member.Name().StringValue());
                // DISTINCT result aliases may read the same input column. Its
                // pre-aggregate definition is shared; result traits stay separate.
                const bool alreadyAdded = member.Struct().Ptr() == lambda.Args().Arg(0).Ptr()
                    && std::any_of(expressionsMapPreAgg.begin(), expressionsMapPreAgg.end(),
                        [&](const auto& entry) {
                            const auto& [name, expression, forceOptional] = entry;
                            if (forceOptional || name.GetFullName() != originalColName.GetFullName()) {
                                return false;
                            }
                            const auto previous = TCoLambda(expression);
                            const auto access = previous.Body().Maybe<TCoMember>();
                            return access && access.Cast().Struct().Ptr() == previous.Args().Arg(0).Ptr()
                                && access.Cast().Name().Value() == member.Name().Value();
                        });
                if (!alreadyAdded) {
                    expressionsMapPreAgg.emplace_back(originalColName.GetFullName(), lambda.Ptr(), false);
                }
'''
    assert before.count(anchor) == 1
    return {SOURCE: (before, before.replace(anchor, replacement, 1))}


def main():
    files = render_files()
    content = ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                      ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                  fromfile='a/' + path, tofile='b/' + path))
                      for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/F01-distinct-input-dedup.patch'
    target.write_text(content)
    with tempfile.TemporaryDirectory(prefix='f01-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        subprocess.run(['git', 'read-tree', BASE], cwd=ROOT.parent, env=env, capture_output=True, check=True)
        checked = subprocess.run(['git', 'apply', '--cached', '--check', '--whitespace=error-all', str(target)],
                                 cwd=ROOT.parent, env=env, capture_output=True)
    report = {'id': 'F01', 'base': BASE, 'patch_sha256': hashlib.sha256(content.encode()).hexdigest(),
              'files': list(files), 'apply_check_passed': checked.returncode == 0,
              'scope': 'Reuse only an existing identical direct-row Member definition for DISTINCT input; retain each result trait and reject malformed Map definitions.',
              'compiled': False, 'native_verified': False, 'production_edited': False,
              'pending': ['Independent source review and fixed native alias profiles.',
                          'Bare-Null DISTINCT on this independent main-based branch still requires the separate C23 fix.']}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': checked.returncode == 0}))
    assert checked.returncode == 0, 'Apply-check failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
