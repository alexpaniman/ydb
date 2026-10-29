"""Compositional native contracts for the ROWS MIN/MAX amendment.

Rendered SQL, rows and ASTs must remain in anonymous memory. This module
generates inputs and independent scalar oracles; it never runs the engine.
"""
import json
import random
import re

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q01_general_rows_minmax_v2'
TYPES = ('Bool', 'Int8', 'Uint8', 'Int16', 'Uint16', 'Int32', 'Uint32', 'Int64', 'Uint64',
         'Decimal', 'Interval', 'Interval64', 'Date', 'Datetime', 'Timestamp',
         'Date32', 'Datetime64', 'Timestamp64')
FRAMES = {'overlap': (-2, 1), 'prior': (-5, -2), 'following': (2, 5),
          'current': (0, 0), 'left': (-4, 0), 'right': (0, 4),
          'wide': (-97, 101), 'emptyhead': (-101, -97), 'emptytail': (97, 101)}
STACK_CALLABLES = ('Prepend', 'Skip', 'TakeWhile', 'Length', 'AggrMin', 'AggrMax')


def _integer(value):
    return g.typed('Int64', value)


def _mod(value, divisor):
    return g.call('Unwrap', g.op(value, '%', _integer(divisor)))


def _type(typ):
    return 'Decimal(22, 3)' if typ == 'Decimal' else typ


def _values(typ):
    if typ == 'Bool':
        return (False, True)
    if typ.startswith('Uint'):
        width = int(typ[4:])
        return (0, 7, 19, (1 << width) - 3)
    if typ.startswith('Int') and typ not in ('Interval', 'Interval64'):
        width = int(typ[3:])
        return (-(1 << (width - 1)) + 3, -7, 19, (1 << (width - 1)) - 3)
    return (0, 1, 2, 3)


def _literal(typ, slot):
    if typ == 'Bool':
        return 'TRUE' if _values(typ)[slot] else 'FALSE'
    if typ == 'Decimal':
        return g.call('Decimal', g.quote(('-17.125', '-0.001', '0.000', '131.999')[slot]), 22, 3)
    if typ in ('Date', 'Date32'):
        return g.typed(typ, ('1970-01-01', '1980-02-03', '2001-03-04', '2020-04-05')[slot])
    if typ in ('Datetime', 'Datetime64', 'Timestamp', 'Timestamp64'):
        value = ('1970-01-01T00:00:00Z', '1980-02-03T01:02:03Z',
                 '2001-03-04T04:05:06Z', '2020-04-05T07:08:09Z')[slot]
        return g.typed(typ, value)
    if typ in ('Interval', 'Interval64'):
        result = g.typed(typ, ('P2D', 'PT1S', 'PT0S', 'P3D')[slot])
        return '-' + result if slot < 2 else result
    return g.typed(typ, _values(typ)[slot])


def _value_expression(typ, nulls):
    size = len(_values(typ))
    value = _literal(typ, size - 1)
    for slot in reversed(range(size - 1)):
        value = g.call('IF', g.op(_mod('id', size), '=', _integer(slot)), _literal(typ, slot), value)
    if nulls == 'required':
        return value
    if nulls == 'allnull':
        return g.call('Nothing', 'Optional<' + _type(typ) + '>')
    return g.call('IF', g.op(_mod('id', 13), '<', _integer(9)),
                  g.call('Nothing', 'Optional<' + _type(typ) + '>'), g.call('Just', value))


def _encoded_result(expression, typ):
    # Compare native typed extrema against their finite input alphabet. This
    # avoids making date/interval/decimal JSON serialization part of the oracle.
    result = _integer(-999)
    for slot in reversed(range(len(_values(typ)))):
        match = g.call('COALESCE', g.op(expression, '=', _literal(typ, slot)), 'FALSE')
        result = g.call('IF', match, _integer(slot), result)
    return g.call('IF', expression + ' IS NULL', g.call('Nothing', 'Int64?'), g.call('Just', result))


def _bound(value):
    return 'CURRENT ROW' if value == 0 else str(abs(value)) + (' PRECEDING' if value < 0 else ' FOLLOWING')


def _specifications():
    result = {}
    for typ in TYPES:
        for nulls in ('required', 'optional', 'allnull'):
            for direction in ('asc', 'desc'):
                name = 'q01_minmax_' + '_'.join((typ.lower(), nulls, direction, 'overlap'))
                result[name] = dict(typ=typ, nulls=nulls, direction=direction, frame='overlap')
    for nulls in ('required', 'optional'):
        for direction in ('asc', 'desc'):
            for frame in FRAMES:
                name = 'q01_minmax_' + '_'.join(('int64', nulls, direction, frame))
                result[name] = dict(typ='Int64', nulls=nulls, direction=direction, frame=frame)
    for label in ('mixed', 'native', 'fallback', 'filter', 'empty', 'range', 'running', 'whole', 'suffix'):
        result['q01_minmax_' + label + '_control'] = dict(typ='Int64', nulls='optional',
                                                       direction='asc', frame='overlap', control=label)
    for nulls in ('required', 'optional', 'allnull'):
        for direction in ('asc', 'desc'):
            for lower in (-5, 2):
                name = 'q01_minmax_general_suffix_' + '_'.join((nulls, direction, 'prior' if lower < 0 else 'following'))
                result[name] = dict(typ='Int64', nulls=nulls, direction=direction,
                                    frame='overlap', control='general_suffix', lower=lower)
    return result


def _query(spec):
    typ, nulls, direction = (spec[key] for key in ('typ', 'nulls', 'direction'))
    control = spec.get('control')
    low, high = FRAMES[spec['frame']]
    frame = 'ROWS BETWEEN ' + _bound(low) + ' AND ' + _bound(high)
    if control == 'range':
        frame = 'RANGE BETWEEN ' + _bound(low) + ' AND ' + _bound(high)
    elif control == 'general_suffix':
        frame = 'ROWS BETWEEN ' + _bound(spec['lower']) + ' AND UNBOUNDED FOLLOWING'
    elif control in ('running', 'whole', 'suffix'):
        frame = {'running': 'ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW',
                 'whole': 'ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING',
                 'suffix': 'ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING'}[control]
    fields = ['id', g.alias(_mod('id', 3), 'p'), g.alias(_mod('id', 5), 'o'),
              g.alias(_value_expression(typ, nulls), 'v')]
    source = g.select(fields, 't', where=g.op('id', '<', _integer(0)) if control == 'empty' else None)
    order = ('o' if control == 'range' else 'id') + ' ' + direction.upper()
    output = ['id', g.alias(g.over(g.call('MIN', 'v'), 'p', order, frame), 'lo'),
              g.alias(g.over(g.call('MAX', 'v'), 'p', order, frame), 'hi')]
    extras = []
    if control in ('mixed', 'fallback'):
        for name, function, argument in [('c', 'COUNT', 'v'), ('s', 'SUM', 'v')]:
            output.append(g.alias(g.over(g.call(function, argument), 'p', order, frame), name))
            extras.append(name)
    if control == 'fallback':
        output.append(g.alias(g.over(g.call('AVG', 'v'), 'p', order, frame), 'a'))
        # Compare a bounded indicator to avoid floating serialization details.
        extras.append(g.alias(g.call('IF', 'a IS NULL', _integer(0), _integer(1)), 'a'))
    if control == 'native':
        output.append(g.alias(g.over(g.call('ROW_NUMBER'), 'p', order), 'r'))
        extras.append('r')
    native = g.select(output, g.derived(source, 'w'))
    return g.select(['id', g.alias(_encoded_result('lo', typ), 'lo'),
                     g.alias(_encoded_result('hi', typ), 'hi')] + extras,
                    g.derived(native, 'r'), order='id',
                    where=g.op(_mod('id', 2), '=', _integer(0)) if control == 'filter' else None)


def recipes(n):
    if type(n) is not int or n < 8:
        raise ValueError('MIN/MAX checks require at least eight rows')
    return {name: _query(spec) for name, spec in _specifications().items()}


def schema_and_seeds(n, case):
    return g.schema_and_seeds(n, case)


def metadata(case, n):
    spec = _specifications()[case]
    control = spec.get('control')
    acceptance = control not in ('empty', 'running', 'whole', 'suffix') and spec['nulls'] != 'allnull'
    settings = {'EnableFallbackToYqlOptimizer': False}
    if control == 'whole':
        settings.update(EnableQueryServiceSpilling=True, EnableSpillingInHashJoinShuffleConnections=True,
                        EnableSpillingNodes='All', DqHashOperatorsUseBlocks=False)
    return {'generator_version': GENERATOR_VERSION, 'table_service_overrides': settings,
            'pragmas': [('ydb.WindowFunctionsV2', 'true')], 'node_count': 1,
            'peak_native_concurrency': 1, 'quality_requires_separate_plan_evidence': True,
            'optimization_acceptance_case': acceptance,
            'additional_ir_admission_required': control == 'range',
            'acceptance_scope': 'General ROWS MIN/MAX plus a RANGE admission control; existing specialized paths remain controls',
            'allocation_scaling_still_required': acceptance}


def _wrap(value):
    value %= 1 << 64
    return value - (1 << 64) if value >= (1 << 63) else value


def expected_rows(case, n):
    spec = _specifications()[case]
    typ, nulls, direction = (spec[key] for key in ('typ', 'nulls', 'direction'))
    control = spec.get('control')
    if control == 'empty':
        return []
    size = len(_values(typ))
    source = [{'id': i, 'p': i % 3, 'o': i % 5,
               'v': None if nulls == 'allnull' or (nulls == 'optional' and i % 13 < 9) else i % size}
              for i in range(1, n + 1)]
    result = []
    for row in source:
        if control == 'filter' and row['id'] % 2:
            continue
        partition = [item for item in source if item['p'] == row['p']]
        partition.sort(key=lambda item: item['id'], reverse=direction == 'desc')
        position = next(i for i, item in enumerate(partition) if item['id'] == row['id'])
        low, high = FRAMES[spec['frame']]
        if control == 'general_suffix':
            low = spec['lower']
        if control == 'range':
            members = [item for item in partition if row['o'] + low <= item['o'] <= row['o'] + high]
        else:
            left = 0 if control in ('running', 'whole') else position if control == 'suffix' else max(0, position + low)
            right = len(partition) - 1 if control in ('whole', 'suffix', 'general_suffix') else position if control == 'running' else min(len(partition) - 1, position + high)
            members = partition[left:right + 1] if left <= right else []
        values = [item['v'] for item in members if item['v'] is not None]
        item = {'id': row['id'], 'lo': min(values) if values else None, 'hi': max(values) if values else None}
        if control in ('mixed', 'fallback'):
            item.update(c=len(values), s=_wrap(sum(_values(typ)[slot] for slot in values)) if values else None)
        if control == 'fallback':
            item['a'] = int(bool(values))
        if control == 'native':
            item['r'] = position + 1
        result.append(item)
    return result


def check_rows(case, rows, n):
    canonical = lambda data: sorted(json.dumps(item, sort_keys=True, separators=(',', ':')) for item in data)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_typed_extrema_oracle': True, 'quality_improvement_verified': False}


def _summary(text):
    summary = windows.physical_ast_summary(text)
    _, nodes = windows.parse_physical_graph(text)
    summary['stack_callables'] = {name: sum(bool(node) and node[0] == name for node in nodes) for name in STACK_CALLABLES}
    summary['range_minmax_calls'] = sum(bool(node) and node[0] == 'KqpRangeMinMax' for node in nodes)
    return summary


def check_physical_ast(case, text, baseline=None):
    spec = _specifications()[case]
    summary = _summary(text)
    counts, stacks = summary['callables'], summary['stack_callables']
    acceptance = metadata(case, 8)['optimization_acceptance_case']
    assertions = {'physical_plan_parsed': True}
    if spec.get('control') == 'range':
        assertions.update(incremental_range_callable_present=summary['range_minmax_calls'] > 0,
                          minmax_only_collector_bypassed=counts['WinFramesCollector'] == 0,
                          no_general_frame_fold=counts['Fold1'] == 0)
    elif acceptance:
        assertions.update(frame_collector_present=counts['WinFramesCollector'] > 0,
                          boundary_reads_present=counts['WinFrame'] >= 2,
                          stack_push_present=stacks['Prepend'] > 0,
                          stack_expiry_present=stacks['Skip'] > 0,
                          initial_or_transfer_fold_present=counts['Fold'] > 0)
        if spec.get('control') == 'fallback':
            assertions['fallback_frame_fold_retained'] = counts['Fold1'] > 0
        else:
            assertions['no_general_frame_fold'] = counts['Fold1'] == 0
    if baseline is not None:
        before = _summary(baseline) if isinstance(baseline, str) else baseline
        if (not isinstance(before, dict) or before.get('physical_ast_parsed') is not True
            or not re.fullmatch('[0-9a-f]{64}', before.get('physical_ast_sha256', ''))
            or set(counts) - set(before.get('callables', {}))
            or any(type(v) is not int or v < 0 for v in before['callables'].values())):
            raise ValueError('Expected validated baseline physical counters')
        summary['baseline_physical_ast_sha256'] = before['physical_ast_sha256']
        summary['baseline_callables'] = before['callables']
        if acceptance and spec.get('control') != 'fallback':
            assertions['baseline_general_frame_fold_present'] = before['callables']['Fold1'] > 0
        if spec.get('control') == 'range':
            assertions['baseline_new_callable_absent'] = before.get('range_minmax_calls') == 0
        if spec.get('control') in ('running', 'whole', 'suffix'):
            assertions['fallback_callable_shape_unchanged'] = counts == before['callables']
    summary.update(structural_assertions=assertions, structural_oracle_pass=all(assertions.values()),
                   optimization_acceptance_case=acceptance, quality_improvement_verified=False,
                   additional_ir_admission_required=spec.get('control') == 'range',
                   allocation_scaling_verified=False)
    return summary


def verify_stack_model(seed=10115, iterations=1500):
    """Independent deque oracle against two-stack placement; counters only."""
    rng = random.Random(seed)
    checked = transfers = appended = 0
    unbounded_cases = 0
    for iteration in range(iterations):
        size = rng.randrange(1, 100)
        values = [None if rng.randrange(4) == 0 else rng.randrange(-1000, 1001) for _ in range(size)]
        lo, hi = sorted((rng.randrange(-120, 121), rng.randrange(-120, 121)))
        unbounded = iteration % 4 == 0
        unbounded_cases += int(unbounded)
        for choose in (min, max):
            combine = lambda a, b: b if a is None else a if b is None else choose(a, b)
            front, rear, previous_end = [], [], -1
            pushed = moved = 0
            for position in range(size):
                first = max(0, position + lo)
                last = size - 1 if unbounded else min(size - 1, position + hi)
                if first > last:
                    front, rear = [], []
                    expected = None
                else:
                    front = [entry for entry in front if entry[0] >= first]
                    if not front:
                        while rear:
                            index, value, _ = rear.pop(0)
                            front.insert(0, (index, value, combine(value, front[0][2] if front else None)))
                            moved += 1
                        front = [entry for entry in front if entry[0] >= first]
                    entries = range(first, last + 1) if previous_end < 0 else ([last] if last > previous_end else [])
                    for index in entries:
                        value = values[index]
                        rear.insert(0, (index, value, combine(rear[0][2] if rear else None, value)))
                        pushed += 1
                    previous_end = last
                    valid = [value for value in values[first:last + 1] if value is not None]
                    expected = choose(valid) if valid else None
                actual = combine(front[0][2] if front else None, rear[0][2] if rear else None)
                if actual != expected or moved > pushed:
                    return {'model_pass': False, 'checked_frames': checked, 'seed': seed}
                checked += 1
            transfers += moved
            appended += pushed
    return {'model_pass': True, 'checked_frames': checked, 'appended_entries': appended,
            'transferred_entries': transfers, 'seed': seed,
            'unbounded_following_partitions': unbounded_cases,
            'allocation_complexity_verified': False}
