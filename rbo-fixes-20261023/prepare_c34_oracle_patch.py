"""Render a strict C34 row-shape oracle amendment without editing active code."""
import difflib
import hashlib
import json
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parent


def render_files():
    before = (ROOT / 'aggregate_checks.py').read_text()
    old = """    if case.startswith('c34_'):
        if len(rows) != 1:
            return False
        value = rows[0]['v']
"""
    new = """    if case.startswith('c34_'):
        expected_columns = {'v', 'a'} if case.endswith('_mixed_avg') else {'v'}
        if len(rows) != 1 or set(rows[0]) != expected_columns:
            return False
        value = rows[0]['v']
"""
    assert before.count(old) == 1
    after = before.replace(old, new)
    old = "and ('a' not in rows[0] or rows[0]['a'] == 1)"
    # C26 has a separate existing condition; change only the C34 branch.
    start = after.index("    if case.startswith('c34_'):")
    assert after[start:].count(old) == 1
    after = after[:start] + after[start:].replace(old, "and (not case.endswith('_mixed_avg') or rows[0]['a'] == 1)")
    return {'aggregate_checks.py': (before, after)}


def main():
    files = render_files()
    before, after = files['aggregate_checks.py']
    patch = 'diff --git a/aggregate_checks.py b/aggregate_checks.py\n' + ''.join(
        difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                             fromfile='a/aggregate_checks.py', tofile='b/aggregate_checks.py'))
    module = ModuleType('prospective_c34_oracle')
    module.__file__ = str(ROOT / 'aggregate_checks.py')
    exec(compile(after, module.__file__, 'exec'), module.__dict__)
    checks = 0
    for kind, value in (('min', 1), ('max', 8), ('some', 1)):
        plain = {'v': [value, 1, value + 1]}
        mixed = dict(plain, a=1)
        assert module.check_rows('c34_' + kind + '_mixed_avg', [mixed], 8)
        assert not module.check_rows('c34_' + kind + '_mixed_avg', [plain], 8)
        assert not module.check_rows('c34_' + kind + '_mixed_avg', [dict(mixed, extra=0)], 8)
        assert not module.check_rows('c34_' + kind + '_mixed_avg', [dict(mixed, a=2)], 8)
        assert module.check_rows('c34_' + kind + '_control', [plain], 8)
        assert not module.check_rows('c34_' + kind + '_control', [mixed], 8)
        assert module.check_rows('c34_' + kind + '_empty', [{'v': None}], 8)
        assert not module.check_rows('c34_' + kind + '_empty', [{'v': None, 'extra': 0}], 8)
        checks += 8
    target = ROOT / 'proposed-correctness-amendments/C34-strict-oracle-fields.patch'
    target.write_text(patch)
    report = {'finding': 'C34', 'status': 'prospective_oracle_contracts_passed_not_applied',
              'source_before_sha256': hashlib.sha256(before.encode()).hexdigest(),
              'source_after_sha256': hashlib.sha256(after.encode()).hexdigest(),
              'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
              'positive_and_negative_contracts': checks, 'active_generator_edited': False,
              'production_sources_edited': False, 'native_executed': False,
              'generated_reproductions_persisted': False}
    (ROOT / 'verification/C34-strict-oracle-artifact-contracts.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
