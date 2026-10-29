"""Q25 demand/outer-join semantic controls; rendered inputs stay in RAM.

These are supplemental controls for the separate 48-case production IR suite.
No SQL result proves that a particular per-definition movement flag was reached.
The unused multirow scalar also covers independent C15 and may still fail on
the main-based Q25-only branch; retain that failure without reclassifying it.
"""
from collections import Counter
import json

import generate_and_verify as g

CASE_SPECIFIC_SCHEMA = True
VERSION = 'q25_map_demand_bag_controls_v1'
CASES = tuple('q25_' + value for value in (
    'filtered_ensure_control', 'conditional_ensure_control',
    'guarded_scalar_singleton_control', 'unused_scalar_multirow_control',
    'demanded_scalar_multirow_error', 'left_outer_bag_control',
    'full_outer_bag_control', 'volatile_shared_value_control'))
EXPECTED_FAILURES = {'q25_demanded_scalar_multirow_error': 'scalar cardinality'}


def _validate(n, case=None):
    if type(n) is not int or n < 8:
        raise ValueError('Map semantics require at least eight rows')
    if case is not None and case not in CASES:
        raise ValueError('Unknown Map semantic control')


def _int(value):
    return g.typed('Int64', value)


def _source(n, side):
    return [{'id': index, 'k': index % 4 if side == 'left' else (4 if index % 3 == 0 else index % 2)}
            for index in range(1, n + 1)]


def schema_and_seeds(n, case):
    _validate(n, case)
    definition = '(id Int64 NOT NULL, k Int64 NOT NULL, PRIMARY KEY (id))'
    schema = ''.join('CREATE TABLE ' + table + ' ' + definition + ';' for table in ('ml', 'mr'))
    seeds = []
    for table, side in (('ml', 'left'), ('mr', 'right')):
        values = [g.select([g.alias(_int(row['id']), 'id'), g.alias(_int(row['k']), 'k')])
                  for row in _source(n, side)]
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(values))
    return schema, seeds


def _outer_join(kind):
    source = g.join('ml AS l', 'mr AS r', g.op('l.k', '=', 'r.k'), kind)
    lower = g.select([g.alias('l.k', 'left_key'), g.alias('r.k', 'right_key'),
                      g.alias(g.call('COALESCE', 'l.k', _int(-1)), 'left_value'),
                      g.alias(g.call('COALESCE', 'r.k', _int(-2)), 'right_value')], source)
    return g.select(['left_key', 'right_key', g.alias(g.op('left_value', '+', _int(10)), 'left_value'),
                     g.alias(g.op('right_value', '+', _int(20)), 'right_value')], g.derived(lower, 'mapped'))


def recipes(n):
    _validate(n)
    result = {}
    cut = _int(n // 2)
    admitted = g.select(['id', 'k'], 'ml', where=g.op('id', '>', cut))
    ensure = lambda column: g.call('Ensure', column, g.op(column, '>', cut), g.quote('map demand guard'))
    for case, source, expression in (
        ('q25_filtered_ensure_control', g.derived(admitted, 'kept'), ensure('kept.id')),
        ('q25_conditional_ensure_control', 'ml AS kept',
         g.call('IF', g.op('kept.id', '>', cut), ensure('kept.id'), _int(-1)))):
        lower = g.select([g.alias('kept.id', 'id'), g.alias(expression, 'guarded')], source)
        result[case] = g.select(['id', g.alias(g.op('guarded', '+', _int(1)), 'value'),
                                g.alias(g.op('id', '+', _int(n)), 'independent')], g.derived(lower, 'mapped'))
    single = '(' + g.select(['id'], 'mr', where=g.op('id', '=', _int(1))) + ')'
    many = '(' + g.select(['id'], 'mr') + ')'
    for case, guard, scalar in (
        ('q25_guarded_scalar_singleton_control', g.op('outer_row.id', '>', cut), single),
        ('q25_unused_scalar_multirow_control', g.op('outer_row.id', '<', _int(0)), many),
        ('q25_demanded_scalar_multirow_error', g.op('outer_row.id', '>', _int(0)), many)):
        lower = g.select([g.alias('outer_row.id', 'id'),
                          g.alias(g.call('IF', guard, scalar, _int(-1)), 'guarded')], 'ml AS outer_row')
        result[case] = g.select(['id', g.alias(g.op('guarded', '+', _int(1)), 'value')], g.derived(lower, 'mapped'))
    result['q25_left_outer_bag_control'] = _outer_join('LEFT')
    result['q25_full_outer_bag_control'] = _outer_join('FULL')
    random_source = g.select(['id', 'k', g.alias(g.call('RandomNumber', 'id'), 'stamp')], 'ml')
    repeated = g.join(g.derived(random_source, 'l'), 'mr AS r', g.op('l.k', '=', 'r.k'))
    lower = g.select([g.alias('l.id', 'left_id'), g.alias('r.id', 'right_id'), g.alias('l.stamp', 'stamp')], repeated)
    result['q25_volatile_shared_value_control'] = g.select(
        ['left_id', 'right_id', 'stamp', g.alias('stamp', 'same_stamp')], g.derived(lower, 'mapped'))
    if tuple(result) != CASES:
        raise ValueError('Map semantic control order changed')
    return result


def metadata(case, n):
    _validate(n, case)
    volatile = case == 'q25_volatile_shared_value_control'
    return {'generator_version': VERSION,
            'table_service_overrides': {'EnableFallbackToYqlOptimizer': False},
            'pragmas': [('kikimr.CostBasedOptimizationLevel', '0'), ('kikimr.HashJoinMode', 'grace')],
            'node_count': 1, 'peak_native_concurrency': 1,
            'repetitions': 4 if volatile else 1,
            'deterministic': not volatile, 'compare_legacy_hash': not volatile,
            'additional_direct_ir_suite_required': 'quality_map_contract_generator:48',
            'rule_path_reachability_proved': False,
            'known_independent_finding': 'C15' if case == 'q25_unused_scalar_multirow_control' else None}


def _join_pairs(n, kind):
    left, right = _source(n, 'left'), _source(n, 'right')
    result, matched_right = [], set()
    for a in left:
        matches = [b for b in right if a['k'] == b['k']]
        for b in matches:
            result.append((a['id'], b['id']))
            matched_right.add(b['id'])
        if not matches and kind in ('LEFT', 'FULL'):
            result.append((a['id'], None))
    if kind == 'FULL':
        result.extend((None, b['id']) for b in right if b['id'] not in matched_right)
    return result


def expected_rows(case, n):
    _validate(n, case)
    if case in EXPECTED_FAILURES or case == 'q25_volatile_shared_value_control':
        raise ValueError('This control has a failure/property oracle, not fixed rows')
    if case in ('q25_left_outer_bag_control', 'q25_full_outer_bag_control'):
        kind = 'LEFT' if case.startswith('q25_left_') else 'FULL'
        left_keys = {row['id']: row['k'] for row in _source(n, 'left')}
        right_keys = {row['id']: row['k'] for row in _source(n, 'right')}
        return [{'left_key': left_keys.get(left), 'right_key': right_keys.get(right),
                 'left_value': (left_keys[left] if left is not None else -1) + 10,
                 'right_value': (right_keys[right] if right is not None else -2) + 20}
                for left, right in _join_pairs(n, kind)]
    result = []
    for index in range(1, n + 1):
        if case == 'q25_filtered_ensure_control' and index <= n // 2:
            continue
        if case in ('q25_filtered_ensure_control', 'q25_conditional_ensure_control'):
            result.append({'id': index, 'value': index + 1 if index > n // 2 else 0,
                           'independent': index + n})
        else:
            result.append({'id': index, 'value': 2 if case == 'q25_guarded_scalar_singleton_control' and index > n // 2 else 0})
    return result


def check_rows(case, rows, n):
    _validate(n, case)
    result = {'semantic_oracle_pass': False, 'query_rule_reachability_verified': False,
              'quality_acceptance_pass': False, 'quality_improvement_verified': False,
              'direct_ir_contracts_verified': False, 'evaluation_count_verified': False}
    if case in EXPECTED_FAILURES:
        result['expected_outcome'] = EXPECTED_FAILURES[case]
        return result
    if case == 'q25_volatile_shared_value_control':
        expected = Counter(_join_pairs(n, 'INNER'))
        valid_shape = all(isinstance(row, dict) and set(row) == {'left_id', 'right_id', 'stamp', 'same_stamp'}
                          and type(row['left_id']) is int and type(row['right_id']) is int
                          and type(row['stamp']) is int and 0 <= row['stamp'] < 2 ** 64
                          and type(row['same_stamp']) is int and row['same_stamp'] == row['stamp'] for row in rows)
        values = {}
        if valid_shape:
            for row in rows:
                values.setdefault(row['left_id'], set()).add(row['stamp'])
        duplicate_groups = sum(count > 1 for count in Counter(left for left, _ in expected.elements()).values())
        result.update(semantic_oracle_pass=valid_shape and bool(expected) and duplicate_groups > 0
                      and Counter((row['left_id'], row['right_id']) for row in rows) == expected
                      and all(len(group) == 1 for group in values.values()),
                      independent_join_bag_oracle=True,
                      shared_value_identity_observed=valid_shape and bool(values) and all(len(group) == 1 for group in values.values()),
                      duplicate_output_groups_required=duplicate_groups,
                      volatile_cross_execution_values_compared=False)
        return result
    canonical = lambda values: Counter(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values)
    result.update(semantic_oracle_pass=canonical(rows) == canonical(expected_rows(case, n)),
                  independent_multiset_oracle=True)
    return result
