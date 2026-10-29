"""Independent integer-SUM models for the Q01 amendment; no native execution.

Generated inputs and expected/actual rows must stay in anonymous memory.
The stored code can generate cases but never writes rendered statements.
"""
import json
import random

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q01_integer_prefix_sums_v1'
TYPES = ('Int8', 'Uint8', 'Int16', 'Uint16', 'Int32', 'Uint32', 'Int64', 'Uint64')


def _typed(typ, value):
    return g.typed(typ, value)


def _mod(value, divisor):
    return g.call('Unwrap', g.op(value, '%', _typed('Int64', divisor)))


def _values(typ):
    width = int(typ[4:] if typ.startswith('Uint') else typ[3:])
    if typ.startswith('Uint'):
        return [(1 << width) - 3, 7, 0, 19]
    return [(1 << (width - 1)) - 3, 7, -(1 << (width - 1)) + 5, -17]


def _expression(typ, nulls):
    choices = _values(typ)
    value = _typed(typ, choices[-1])
    for slot in reversed(range(3)):
        value = g.call('IF', g.op(_mod('id', 4), '=', _typed('Int64', slot)), _typed(typ, choices[slot]), value)
    if nulls == 'required':
        return value
    if nulls == 'allnull':
        return g.call('Nothing', typ + '?')
    return g.call('IF', g.op(_mod('id', 9), '<', _typed('Int64', 6)),
                  g.call('Nothing', typ + '?'), g.call('Just', value))


def _query(typ, nulls, kind, direction, prior, *, native=False, empty=False, frame_override=None):
    fields = ['id', g.alias(_mod('id', 3), 'p'), g.alias(_mod('id', 5), 'o'), g.alias(_expression(typ, nulls), 'v')]
    source = g.select(fields, 't', where=g.op('id', '<', _typed('Int64', 0)) if empty else None)
    order = ('id' if kind == 'rows' else 'o') + ' ' + direction.upper()
    frame = frame_override or kind.upper() + ' BETWEEN 2 PRECEDING AND ' + ('1 PRECEDING' if prior else '1 FOLLOWING')
    output = ['id', g.alias(g.over(g.call('SUM', 'v'), 'p', order, frame), 's')]
    if native:
        output.append(g.alias(g.over(g.call('ROW_NUMBER'), 'p', order), 'r'))
    return g.select(output, g.derived(source, 'w'), order='id')


def recipes(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('SUM checks require at least eight rows')
    result = {}
    for typ in TYPES:
        for nulls in ('required', 'optional', 'allnull'):
            for kind in ('rows', 'range'):
                for direction in ('asc', 'desc'):
                    for prior in (False, True):
                        case = 'q01_sum_' + '_'.join((typ.lower(), nulls, kind, direction, 'prior' if prior else 'overlap'))
                        result[case] = _query(typ, nulls, kind, direction, prior)
    result['q01_sum_native_control'] = _query('Int64', 'optional', 'rows', 'asc', False, native=True)
    result['q01_sum_empty_control'] = _query('Int64', 'optional', 'rows', 'asc', False, empty=True)
    for label, frame in [('running', 'ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW'),
                         ('whole', 'ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING'),
                         ('suffix', 'ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING')]:
        result['q01_sum_' + label + '_control'] = _query('Int64', 'optional', 'rows', 'asc', False, frame_override=frame)
    return result


def schema_and_seeds(n, case):
    return g.schema_and_seeds(n, case)


def metadata(case, n):
    settings = {'EnableFallbackToYqlOptimizer': False}
    if case == 'q01_sum_whole_control':
        settings.update(EnableQueryServiceSpilling=True, EnableSpillingInHashJoinShuffleConnections=True,
                        EnableSpillingNodes='All', DqHashOperatorsUseBlocks=False)
    return {'generator_version': GENERATOR_VERSION, 'table_service_overrides': settings,
            'pragmas': [('ydb.WindowFunctionsV2', 'true')], 'node_count': 1,
            'peak_native_concurrency': 1, 'quality_requires_separate_plan_evidence': True,
            'optimization_acceptance_case': not case.endswith('_control') or case == 'q01_sum_native_control',
            'acceptance_scope': 'Integer-SUM amendment, not the earlier COUNT-only branch'}


def _wrap(value, signed):
    value %= 1 << 64
    return value - (1 << 64) if signed and value >= (1 << 63) else value


def ordered_sum(values, signed):
    result = None
    for value in values:
        if value is not None:
            result = value if result is None else _wrap(result + value, signed)
    return result


def expected_rows(case, n):
    if case == 'q01_sum_empty_control':
        return []
    if case.endswith('_control'):
        typ, nulls, kind, direction, frame = 'Int64', 'optional', 'rows', 'asc', case.split('_')[2]
    else:
        _, _, typ_lower, nulls, kind, direction, frame = case.split('_')
        typ = next(candidate for candidate in TYPES if candidate.lower() == typ_lower)
    values = _values(typ)
    source = [{'id': i, 'p': i % 3, 'o': i % 5,
               'v': None if nulls == 'allnull' or (nulls == 'optional' and i % 9 < 6) else values[i % 4]}
              for i in range(1, n + 1)]
    output = []
    for row in source:
        partition = [item for item in source if item['p'] == row['p']]
        if kind == 'rows':
            partition.sort(key=lambda item: item['id'], reverse=direction == 'desc')
            position = next(index for index, item in enumerate(partition) if item['id'] == row['id'])
            low = 0 if frame in ('running', 'whole') else position if frame == 'suffix' else max(0, position - 2)
            high = len(partition) - 1 if frame in ('whole', 'suffix') else position if frame == 'running' else min(
                len(partition) - 1, position + (-1 if frame == 'prior' else 1))
            members = partition[low:high + 1] if low <= high else []
        else:
            sign = -1 if direction == 'desc' else 1
            point = sign * row['o']
            members = [item for item in partition if point - 2 <= sign * item['o'] <= point + (-1 if frame == 'prior' else 1)]
        item = {'id': row['id'], 's': ordered_sum([member['v'] for member in members], typ.startswith('Int'))}
        if case == 'q01_sum_native_control':
            item['r'] = position + 1
        output.append(item)
    return output


def check_rows(case, rows, n):
    canonical = lambda data: sorted(json.dumps(item, sort_keys=True, separators=(',', ':')) for item in data)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_ordered_modular_sum_oracle': True, 'quality_improvement_verified': False}


def check_physical_ast(case, text, baseline=None):
    # General-frame SUM/native cases must remove the per-frame fold. Existing
    # running/whole/suffix specializations are semantic and shape controls.
    if not case.endswith('_control') or case == 'q01_sum_native_control':
        return windows.check_physical_ast('q01_rows_asc_overlap', text, baseline)
    result = windows.check_physical_ast('q01_empty_control', text, baseline)
    assertions = {'control_physical_plan_parsed': result['physical_ast_parsed']}
    if baseline is not None:
        assertions['control_callable_shape_unchanged'] = result['callables'] == result['baseline_callables']
    result.update(structural_assertions=assertions, structural_oracle_pass=all(assertions.values()),
                  optimization_acceptance_case=False)
    return result


def verify_prefix_algebra(seed=173, iterations=1000):
    """Exercise the algebra with independent ordered folds; return counts only."""
    randomizer = random.Random(seed)
    checked = 0
    for signed in (False, True):
        for _ in range(iterations):
            size = randomizer.randrange(1, 65)
            values = [None if randomizer.randrange(4) == 0 else _wrap(randomizer.getrandbits(64), signed)
                      for _ in range(size)]
            prefix_sums, prefix_counts = [], []
            total, count = 0, 0
            for value in values:
                if value is not None:
                    total = _wrap(total + value, signed)
                    count += 1
                prefix_sums.append(total)
                prefix_counts.append(count)
            for _ in range(16):
                left, right = sorted((randomizer.randrange(size), randomizer.randrange(size)))
                first = values[left]
                count = prefix_counts[right] - prefix_counts[left] + (first is not None)
                summed = _wrap(prefix_sums[right] - prefix_sums[left] + (first or 0), signed) if count else None
                if summed != ordered_sum(values[left:right + 1], signed):
                    return {'algebra_oracle_pass': False, 'checked_frames': checked, 'seed': seed}
                checked += 1
    return {'algebra_oracle_pass': True, 'checked_frames': checked, 'seed': seed}
