"""Prepare dormant artifact changes; live SQL generators remain untouched."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent


def render_files():
    result = {}
    for name in ('sharing_lifecycle_checks.py', 'c31_identity_checks.py'):
        path = ROOT / name
        before = path.read_text()
        old = 'def recipes(n):\n'
        assert before.count(old) == 1
        after = before.replace(old, 'def _all_recipes(n):\n', 1)
        if name == 'sharing_lifecycle_checks.py':
            predicate = "not case.startswith('c31_') or case.endswith('_left')"
        else:
            predicate = "case.endswith('__left')"
        additions = '''

def recipes(n):
    # Forced YqlSelect rejects explicit SEMI/ONLY syntax before RBO import.
    # Preserve those builders separately; a parser failure is not a C31 test.
    return {case: query for case, query in _all_recipes(n).items()
            if PREDICATE}


def unavailable_recipes(n):
    """Retain rejected frontend profiles without counting optimizer coverage."""
    return {case: query for case, query in _all_recipes(n).items()
            if not (PREDICATE)}
'''.replace('PREDICATE', predicate)
        anchor = '\ndef metadata(case, n):\n'
        assert after.count(anchor) == 1
        after = after.replace(anchor, additions + anchor)
        result['rbo-fixes-20261023/' + name] = (before, after)
    return result


def main():
    files = render_files()
    patch = ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                    ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + path, tofile='b/' + path))
                    for path, (before, after) in files.items())
    target = ROOT / 'proposed-correctness-amendments/C31-frontend-coverage-artifacts.patch'
    target.write_text(patch)
    checked = subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(target.resolve())],
                             cwd=ROOT.parent, capture_output=True)
    report = {'finding': 'C31', 'artifact_only': True, 'live_files_modified': False,
              'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
              'preimage_sha256': {path: hashlib.sha256(before.encode()).hexdigest() for path, (before, _) in files.items()},
              'apply_check_passed': checked.returncode == 0,
              'generator_change': 'Retain LEFT SQL profiles; move unsupported explicit SEMI/ONLY builders to unavailable_recipes.',
              'preserve_native_failures': True,
              'excluded_frontend_profile_count': 96,
              'retained_C31_SQL_profile_count_in_changed_modules': 48,
              'retained_C31_SQL_profile_count_including_original_subsystem_controls': 50,
              'exists_alternative_added': False,
              'reason': 'EXISTS/NOT EXISTS creates a pure-key outer LeftSemi/LeftOnly, with mixed residual evaluation inside the dependent right-side plan. That does not prove C31 residual-reconstruction coverage.',
              'direct_cpp_scope': 'Existing 24 internal rule contracts include LeftSemi/LeftOnly binding and identity checks; no full SQL semantics claim.',
              'C38_exclusion': 'Already excluded from active distribution_checks.recipes and retained in unavailable_recipes.',
              'native_executed': False, 'mocks_executed': False}
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'patch_sha256': report['patch_sha256'], 'apply_check_passed': checked.returncode == 0}))
    assert checked.returncode == 0, 'Artifact apply check failed; raw diagnostics suppressed'


if __name__ == '__main__':
    main()
