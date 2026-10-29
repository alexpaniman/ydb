"""Prepare dormant quality-pairing changes without editing the active driver."""
import difflib
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('pairing_patch_anchor_is_not_unique')
    return text.replace(old, new, 1)


RANGE_COUNT = '''def _range_calls(record):
    value = record.get('range_minmax_calls')
    if type(value) is not int or value < 0:
        raise ValueError('Missing or invalid RANGE callable count')
    return value


'''

RANGE_ACCEPTANCE = '''    case = after.get('case', '')
    if case == 'q01_minmax_range_control' or case.startswith('q01_range_minmax_'):
        if after.get('optimization_acceptance_case') is True:
            assertions = after.get('structural_assertions')
            return (isinstance(assertions, dict) and bool(assertions)
                    and all(value is True for value in assertions.values())
                    and assertions.get('incremental_range_callable_present') is True
                    and _range_calls(before) == 0 and _range_calls(after) > 0
                    and _count(before, 'Fold1') > 0)
        if case.startswith(tuple('q01_range_minmax_' + name + '_' for name in ('running', 'whole', 'float_value'))):
            return (after.get('structural_assertions', {}).get('physical_plan_parsed') is True
                    and _range_calls(before) == _range_calls(after) == 0
                    and isinstance(before.get('callables'), dict) and bool(before['callables'])
                    and before['callables'] == after.get('callables'))
'''

RANGE_TESTS = '''    range_before = {'case': 'q01_range_minmax_int64_optional_asc_overlap',
                    'range_minmax_calls': 0, 'callables': {'Fold1': 1}}
    range_after = {'case': range_before['case'], 'optimization_acceptance_case': True,
                   'range_minmax_calls': 1, 'callables': {'Fold1': 0},
                   'structural_assertions': {'physical_plan_parsed': True,
                       'incremental_range_callable_present': True,
                       'remaining_general_fold_policy': True, 'minmax_only_collector_bypassed': True}}
    check(pairing._fixed_acceptance(range_before, range_after))
    check(pairing._improvement(range_after['case'], range_before, range_after))
    check(not pairing._fixed_acceptance(range_before, dict(range_after, range_minmax_calls=0)))
    check(not pairing._improvement(range_after['case'], range_before, dict(range_after, range_minmax_calls=0)))
    check(not pairing._fixed_acceptance(dict(range_before, range_minmax_calls=1), range_after))
    check(not pairing._fixed_acceptance(range_before, dict(range_after, structural_assertions={'physical_plan_parsed': True})))
    check(not pairing._improvement(range_after['case'], range_before, dict(range_after, optimization_acceptance_case=False)))
    fallback = dict(range_after, case='q01_range_minmax_float_value_asc',
                    optimization_acceptance_case=False, range_minmax_calls=0, callables={'Fold1': 1},
                    structural_assertions={'physical_plan_parsed': True, 'unsupported_scope_not_admitted': True})
    check(pairing._fixed_acceptance(range_before, fallback))
    check(not pairing._fixed_acceptance(range_before, dict(fallback, callables={'Fold1': 0})))
    check(pairing._fixed_acceptance(range_before, dict(range_after, case='q01_minmax_range_control')))
'''


def render_files():
    path = HERE / 'quality_pairing.py'
    before = path.read_text()
    after = _once(before, "    'minmax': ('quality_minmax_checks',),", "    'minmax': ('quality_minmax_checks',), 'range_minmax': ('quality_range_minmax_checks',),")
    after = _once(after, "'Q01': ('window', 'sum', 'minmax')", "'Q01': ('window', 'sum', 'minmax', 'range_minmax')")
    after = _once(after, 'def _sum_field(record, collection, field):', RANGE_COUNT + 'def _sum_field(record, collection, field):')
    anchor = "    if case.startswith('q01_minmax_') and after.get('structural_assertions', {}).get('no_general_frame_fold'):"
    after = _once(after, anchor, "    if case.startswith('q01_range_minmax_'):\n        return (after.get('optimization_acceptance_case') is True\n                and after.get('structural_assertions', {}).get('incremental_range_callable_present') is True\n                and _range_calls(before) == 0 and _range_calls(after) > 0\n                and _count(before, 'Fold1') > 0)\n" + anchor)
    after = _once(after, 'def _fixed_acceptance(before, after):\n', 'def _fixed_acceptance(before, after):\n' + RANGE_ACCEPTANCE)
    after = _once(after, "for kind in ('range', 'running', 'whole', 'suffix')", "for kind in ('running', 'whole', 'suffix')")
    result = {path.name: (before, after)}
    path = HERE / 'quality_pairing_contracts.py'
    before = path.read_text()
    after = _once(before, "['quality_window_checks', 'quality_sum_checks', 'quality_minmax_checks']", "['quality_window_checks', 'quality_sum_checks', 'quality_minmax_checks', 'quality_range_minmax_checks']")
    after = _once(after, "control = {'case': 'q01_minmax_range_control'", "control = {'case': 'q01_minmax_running_control'")
    after = _once(after, '    ir_before, ir_after = copy.deepcopy(before), copy.deepcopy(after)', RANGE_TESTS + '    ir_before, ir_after = copy.deepcopy(before), copy.deepcopy(after)')
    result[path.name] = (before, after)
    return result


def render_patch():
    return ''.join('diff --git a/' + name + ' b/' + name + '\n' +
                   ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + name, tofile='b/' + name))
                   for name, (before, after) in render_files().items())
