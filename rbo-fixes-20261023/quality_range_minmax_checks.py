"""Advancing RANGE MIN/MAX generators and independent oracles; payloads stay in RAM."""
from collections import deque
import json
import random
import re

import generate_and_verify as g
import quality_minmax_checks as typed
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q01_advancing_range_minmax_v2'
FRAMES = typed.FRAMES | {'suffix_prior': (-5, None), 'suffix_current': (0, None), 'suffix_following': (5, None)}


def _specifications():
    result = {}
    for typ in typed.TYPES:
        for nulls in ('required', 'optional', 'allnull'):
            for direction in ('asc', 'desc'):
                name = 'q01_range_minmax_' + '_'.join((typ.lower(), nulls, direction, 'overlap'))
                result[name] = dict(typ=typ, nulls=nulls, direction=direction, frame='overlap', key='peers')
    for direction in ('asc', 'desc'):
        for frame in FRAMES:
            for key in ('peers', 'sparse', 'nullable_first', 'nullable_last'):
                name = 'q01_range_minmax_' + '_'.join(('int64', 'optional', direction, frame, key))
                result[name] = dict(typ='Int64', nulls='optional', direction=direction, frame=frame, key=key)
    for control in ('mixed', 'fallback', 'native', 'filter', 'empty', 'running', 'whole', 'suffix', 'float_value'):
        for direction in ('asc', 'desc'):
            result['q01_range_minmax_' + control + '_' + direction] = dict(
                typ='Double' if control == 'float_value' else 'Int64', nulls='optional',
                direction=direction, frame='overlap', key='peers', control=control)
    for frame in ('suffix_prior', 'suffix_current', 'suffix_following'):
        for direction in ('asc', 'desc'):
            for key in ('peers', 'sparse', 'nullable_first', 'nullable_last'):
                result['q01_range_minmax_required_' + '_'.join((frame, direction, key))] = dict(
                    typ='Int64', nulls='required', direction=direction, frame=frame, key=key)
            for control in ('mixed', 'fallback', 'native'):
                for key in ('sparse', 'nullable_first'):
                    result['q01_range_minmax_' + '_'.join((control, frame, direction, key))] = dict(
                        typ='Int64', nulls='optional', direction=direction, frame=frame, key=key, control=control)
    return result


def _key_expression(spec):
    key = typed._mod('id', 7)
    if spec['key'] == 'sparse':
        return g.op(key, '*', typed._integer(17))
    if spec['key'].startswith('nullable'):
        return g.call('IF', g.op(typed._mod('id', 5), '=', typed._integer(0)),
                      g.call('Nothing', 'Int64?'), g.call('Just', key))
    return key


def _query(spec):
    typ, direction = spec['typ'], spec['direction']
    control = spec.get('control')
    low, high = FRAMES[spec['frame']]
    frame = 'RANGE BETWEEN ' + typed._bound(low) + ' AND ' + ('UNBOUNDED FOLLOWING' if high is None else typed._bound(high))
    if control in ('running', 'whole', 'suffix'):
        frame = {'running': 'RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW',
                 'whole': 'RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING',
                 'suffix': 'RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING'}[control]
    fields = ['id', g.alias(typed._mod('id', 3), 'p'), g.alias(_key_expression(spec), 'o'),
              g.alias(typed._value_expression(typ, spec['nulls']), 'v')]
    source = g.select(fields, 't', where=g.op('id', '<', typed._integer(0)) if control == 'empty' else None)
    order = 'o ' + direction.upper()
    if spec['key'].startswith('nullable'):
        order += ' NULLS ' + ('FIRST' if spec['key'].endswith('first') else 'LAST')
    output = ['id', g.alias(g.over(g.call('MIN', 'v'), 'p', order, frame), 'lo'),
              g.alias(g.over(g.call('MAX', 'v'), 'p', order, frame), 'hi')]
    extras = []
    if control in ('mixed', 'fallback'):
        for name, function in [('c', 'COUNT'), ('s', 'SUM')]:
            output.append(g.alias(g.over(g.call(function, 'v'), 'p', order, frame), name))
            extras.append(name)
    if control == 'fallback':
        output.append(g.alias(g.over(g.call('AVG', 'v'), 'p', order, frame), 'a'))
        extras.append(g.alias(g.call('IF', 'a IS NULL', typed._integer(0), typed._integer(1)), 'a'))
    if control == 'native':
        output.append(g.alias(g.over(g.call('RANK'), 'p', order, frame), 'r'))
        extras.append('r')
    result = g.select(output, g.derived(source, 'w'))
    return g.select(['id', g.alias(typed._encoded_result('lo', typ), 'lo'),
                     g.alias(typed._encoded_result('hi', typ), 'hi')] + extras,
                    g.derived(result, 'r'), order='id',
                    where=g.op(typed._mod('id', 2), '=', typed._integer(0)) if control == 'filter' else None)


def recipes(n):
    if type(n) is not int or n < 8:
        raise ValueError('RANGE checks require at least eight rows')
    return {name: _query(spec) for name, spec in _specifications().items()}


def schema_and_seeds(n, case):
    return g.schema_and_seeds(n, case)


def metadata(case, n):
    spec = _specifications()[case]
    control = spec.get('control')
    accepted = control not in ('empty', 'running', 'whole', 'float_value') and spec['nulls'] != 'allnull'
    settings = {'EnableFallbackToYqlOptimizer': False}
    if control == 'whole':
        settings.update(EnableQueryServiceSpilling=True, EnableSpillingInHashJoinShuffleConnections=True,
                        EnableSpillingNodes='All', DqHashOperatorsUseBlocks=False)
    return {'generator_version': GENERATOR_VERSION, 'table_service_overrides': settings,
            'pragmas': [('ydb.WindowFunctionsV2', 'true')], 'node_count': 1, 'peak_native_concurrency': 1,
            'quality_requires_separate_plan_evidence': True, 'optimization_acceptance_case': accepted,
            'additional_ir_admission_required': accepted,
            'acceptance_scope': 'Finite-lower RANGE MIN/MAX, bounded or unbounded upper endpoint, with separate runtime callable evidence',
            'allocation_scaling_still_required': accepted, 'yield_and_early_release_native_checks_still_required': accepted}


def _rows(spec, n):
    size = len(typed._values(spec['typ']))
    return [{'id': i, 'p': i % 3,
             'o': None if spec['key'].startswith('nullable') and i % 5 == 0 else (i % 7) * (17 if spec['key'] == 'sparse' else 1),
             'v': None if spec['nulls'] == 'allnull' or (spec['nulls'] == 'optional' and i % 13 < 9) else i % size}
            for i in range(1, n + 1)]


def _sortkey(row, spec):
    null_first = spec['key'].endswith('first')
    if row['o'] is None:
        return (0 if null_first else 1, 0)
    return (1 if null_first else 0, row['o'] * (1 if spec['direction'] == 'asc' else -1))


def _members(row, partition, spec):
    control = spec.get('control')
    if control == 'whole':
        return partition
    if control == 'running':
        return [item for item in partition if _sortkey(item, spec) <= _sortkey(row, spec)]
    if control == 'suffix':
        return [item for item in partition if _sortkey(item, spec) >= _sortkey(row, spec)]
    low, high = FRAMES[spec['frame']]
    if row['o'] is None:
        return [item for item in partition if item['o'] is None or
                (high is None and _sortkey(item, spec) >= _sortkey(row, spec))]
    sign = 1 if spec['direction'] == 'asc' else -1
    if high is None:
        # Unbounded following includes every later NULL group as well.
        return [item for item in partition if
                (item['o'] is not None and low <= sign * (item['o'] - row['o'])) or
                (item['o'] is None and _sortkey(item, spec) >= _sortkey(row, spec))]
    return [item for item in partition if item['o'] is not None and low <= sign * (item['o'] - row['o']) <= high]


def expected_rows(case, n):
    spec = _specifications()[case]
    control = spec.get('control')
    if control == 'empty':
        return []
    source, result = _rows(spec, n), []
    for row in source:
        if control == 'filter' and row['id'] % 2:
            continue
        partition = [item for item in source if item['p'] == row['p']]
        values = [item['v'] for item in _members(row, partition, spec) if item['v'] is not None]
        item = {'id': row['id'], 'lo': min(values) if values else None, 'hi': max(values) if values else None}
        if control in ('mixed', 'fallback'):
            item.update(c=len(values), s=typed._wrap(sum(typed._values(spec['typ'])[slot] for slot in values)) if values else None)
        if control == 'fallback':
            item['a'] = int(bool(values))
        if control == 'native':
            item['r'] = 1 + sum(_sortkey(other, spec) < _sortkey(row, spec) for other in partition)
        result.append(item)
    return result


def check_rows(case, rows, n):
    canonical = lambda items: sorted(json.dumps(item, sort_keys=True, separators=(',', ':')) for item in items)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_typed_extrema_oracle': True, 'quality_improvement_verified': False}


def _summary(text):
    result = windows.physical_ast_summary(text)
    _, nodes = windows.parse_physical_graph(text)
    result['range_minmax_calls'] = sum(bool(node) and node[0] == 'KqpRangeMinMax' for node in nodes)
    return result


def check_physical_ast(case, text, baseline=None):
    spec, summary = _specifications()[case], _summary(text)
    accepted = metadata(case, 8)['optimization_acceptance_case']
    control = spec.get('control')
    counts = summary['callables']
    assertions = {'physical_plan_parsed': True}
    if accepted:
        assertions['incremental_range_callable_present'] = summary['range_minmax_calls'] > 0
        assertions['remaining_general_fold_policy'] = counts['Fold1'] > 0 if control == 'fallback' else counts['Fold1'] == 0
        if control not in ('mixed', 'fallback', 'native'):
            assertions['minmax_only_collector_bypassed'] = counts['WinFramesCollector'] == 0
        elif control in ('mixed', 'fallback', 'native'):
            assertions['other_function_collector_retained'] = counts['WinFramesCollector'] > 0
    if control in ('running', 'whole', 'float_value'):
        assertions['unsupported_scope_not_admitted'] = summary['range_minmax_calls'] == 0
    if baseline is not None:
        before = _summary(baseline) if isinstance(baseline, str) else baseline
        if (not isinstance(before, dict) or before.get('physical_ast_parsed') is not True
            or not re.fullmatch('[0-9a-f]{64}', before.get('physical_ast_sha256', ''))
            or type(before.get('range_minmax_calls')) is not int or before['range_minmax_calls'] < 0
            or set(counts) - set(before.get('callables', {}))
            or any(type(value) is not int or value < 0 for value in before['callables'].values())):
            raise ValueError('Expected validated baseline physical counters')
        summary['baseline_physical_ast_sha256'] = before['physical_ast_sha256']
        summary['baseline_range_minmax_calls'] = before['range_minmax_calls']
        summary['baseline_callables'] = before['callables']
        if accepted:
            assertions['baseline_new_callable_absent'] = before['range_minmax_calls'] == 0
            if control != 'fallback':
                assertions['baseline_general_fold_present'] = before['callables']['Fold1'] > 0
        if control in ('running', 'whole', 'float_value'):
            assertions['fallback_callable_shape_unchanged'] = counts == before['callables']
    summary.update(structural_assertions=assertions, structural_oracle_pass=all(assertions.values()),
                   optimization_acceptance_case=accepted, quality_improvement_verified=False,
                   additional_ir_admission_required=accepted,
                   allocation_scaling_verified=False)
    return summary


def verify_deque_model(seed=19281, iterations=240):
    rng = random.Random(seed)
    frames_checked = inserted = removed = 0
    for _ in range(iterations):
        before_inserted = inserted
        n = rng.randrange(1, 180)
        keys = sorted(rng.randrange(-90, 91) for _ in range(n))
        values = [None if rng.randrange(3) == 0 else rng.randrange(-30, 31) for _ in keys]
        low, high = sorted((rng.randrange(-35, 36), rng.randrange(-35, 36)))
        if rng.randrange(4) == 0:
            high = None
        minimum, maximum, seen_end = deque(), deque(), 0
        for key in keys:
            begin = next((i for i, item in enumerate(keys) if item >= key + low), n)
            end = n if high is None else next((i for i, item in enumerate(keys) if item > key + high), n)
            for candidates, is_min in ((minimum, True), (maximum, False)):
                while candidates and candidates[0][0] < begin:
                    candidates.popleft()
                    removed += 1
                if begin >= end:
                    removed += len(candidates)
                    candidates.clear()
                else:
                    for ordinal in range(max(seen_end, begin), end):
                        value = values[ordinal]
                        if value is None:
                            continue
                        while candidates and (candidates[-1][1] >= value if is_min else candidates[-1][1] <= value):
                            candidates.pop()
                            removed += 1
                        candidates.append((ordinal, value))
                        inserted += 1
                expected = [value for value in values[begin:end] if value is not None]
                assert (candidates[0][1] if candidates else None) == ((min(expected) if is_min else max(expected)) if expected else None)
                frames_checked += 1
            seen_end = max(seen_end, begin, end)
        assert inserted >= removed
        assert inserted - before_inserted <= 2 * n
    return {'model_pass': True, 'seed': seed, 'frames_checked': frames_checked,
            'candidate_insertions': inserted, 'candidate_removals': removed,
            'native_collector_invoked': False, 'native_runtime_invoked': False}
