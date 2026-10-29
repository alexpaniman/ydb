"""Compositional C09-C14 inputs and sanitized semantic oracles.

Generate query text only in memory. No execution or persistence occurs here.
Use metadata flag_pragmas to select explicit IN semantics in both optimizers.
"""
import importlib.util
from pathlib import Path

_REVIEW = Path(__file__).resolve().parent.parent / 'rbo-review-20261021'
_SPEC = importlib.util.spec_from_file_location('_expression_builders', _REVIEW / 'generate_and_verify.py')
g = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(g)
schema_and_seeds = g.schema_and_seeds

CASES = {
    'c09_legacy_null_rhs': 'C09', 'c09_ansi_null_rhs': 'C09',
    'c09_legacy_null_lookup_empty': 'C09', 'c09_ansi_null_lookup_empty': 'C09',
    'c09_matching_control': 'C09',
    'c10_nan_in': 'C10', 'c10_value_in': 'C10',
    'c10_null_in': 'C10', 'c10_nan_empty': 'C10',
    'c11_nullable_rhs_fallback': 'C11', 'c11_nullable_lookup_fallback': 'C11',
    'c11_nonnullable_control': 'C11', 'c11_exists_control': 'C11',
    'c12_hidden_group_key': 'C12', 'c12_visible_group_key': 'C12',
    'c12_no_distinct': 'C12',
    'c13_aggregate_group_expression': 'C13', 'c13_distinct_group_expression': 'C13',
    'c13_direct_group_control': 'C13', 'c13_plain_group_expression': 'C13',
    'c14_nested_receivers': 'C14', 'c14_deep_receivers': 'C14',
    'c14_direct_control': 'C14', 'c14_nested_having': 'C14',
}


def _subquery(query):
    return '(' + query + ')'


def _membership(source, probe, rhs, *, negated=False, fallback=False):
    predicate = g.op(probe, 'NOT IN' if negated else 'IN', _subquery(rhs))
    if fallback:
        predicate = g.call('COALESCE', predicate, 'TRUE')
    return g.select(['id'], source, where=predicate)


def _nested_source(deep=False):
    def structure(value):
        item = g.call('AsStruct', g.alias(g.typed('Int64', value), 'x'))
        return g.call('AsStruct', g.alias(item, 'inner')) if deep else item
    return g.derived(g.select([g.alias(structure(1), 's'), g.alias(structure(2), 'q')], 'l'), 'z')


def recipes(n):
    if not isinstance(n, int) or n < 2:
        raise ValueError('Use an integer fixture size of at least two')
    result = {}
    null_rhs = g.select([g.alias(g.call('Nothing', 'Int32?'), 'v')])
    empty_rhs = g.select(['id'], 'l', where=g.op('id', '<', g.typed('Int32', 0)))
    null_lookup = g.derived(g.select(['id', g.alias(g.call('Nothing', 'Int32?'), 'probe')], 'l'), 's')
    for mode in ('legacy', 'ansi'):
        result['c09_' + mode + '_null_rhs'] = _membership('l', 'id', null_rhs, negated=True)
        result['c09_' + mode + '_null_lookup_empty'] = _membership(null_lookup, 'probe', empty_rhs, negated=True)
    result['c09_matching_control'] = _membership('l', 'id', g.select(['id'], 'l'))

    finite_rhs = g.select([g.alias(g.typed('Double', 2), 'v')])
    empty_finite_rhs = g.select([g.alias(g.typed('Double', 2), 'v')], 't', where=g.op('id', '<', '0'))
    for label, value, empty in [('nan_in', 'nan', False), ('value_in', 3, False),
                                ('null_in', None, False), ('nan_empty', 'nan', True)]:
        probe = (g.call('Nothing', 'Double?') if value is None else
                 g.call('IF', g.op('id', '>', '0'), g.typed('Double', value), g.call('Nothing', 'Double?')))
        source = g.derived(g.select(['id', g.alias(probe, 'probe')], 'r'), 's')
        result['c10_' + label] = _membership(source, 'probe', empty_finite_rhs if empty else finite_rhs, negated=True)

    result['c11_nullable_rhs_fallback'] = _membership('l', 'id', null_rhs, fallback=True)
    result['c11_nullable_lookup_fallback'] = _membership(null_lookup, 'probe', g.select(['id'], 'l'), fallback=True)
    result['c11_nonnullable_control'] = _membership('l', 'id', g.select(['id'], 'l'), fallback=True)
    exists = 'EXISTS ' + _subquery(g.select(['id'], 'l'))
    result['c11_exists_control'] = g.select(['id'], 'l', where=g.call('COALESCE', exists, 'FALSE'))

    count = g.call('COUNT', '*')
    result['c12_hidden_group_key'] = g.select([g.alias(count, 'c')], 't', group='id', distinct=True)
    result['c12_visible_group_key'] = g.select(['id', g.alias(count, 'c')], 't', group='id', distinct=True)
    result['c12_no_distinct'] = g.select([g.alias(count, 'c')], 't', group='id')
    grouped_expression = g.call('COALESCE', g.op('k', '+', g.call('CAST', count + ' AS Int64')), 'k')
    result['c13_aggregate_group_expression'] = g.select([g.alias(grouped_expression, 'v')], 't', group='k')
    result['c13_distinct_group_expression'] = g.select([g.alias(g.op('id', '+', g.typed('Int64', 10)), 'v')], 't', group='id', distinct=True)
    result['c13_direct_group_control'] = g.select([g.alias('k', 'v')], 't', group='k')
    result['c13_plain_group_expression'] = g.select([g.alias(g.op('k', '+', g.typed('Int64', 10)), 'v')], 't', group='k')
    for deep in (False, True):
        suffix = '.inner.x' if deep else '.x'
        fields = [g.alias(g.call('SUM', 'z.s' + suffix), 'a'), g.alias(g.call('SUM', 'z.q' + suffix), 'b')]
        result['c14_deep_receivers' if deep else 'c14_nested_receivers'] = g.select(fields, _nested_source(deep))
        if not deep:
            result['c14_nested_having'] = g.select(fields, _nested_source(),
                having=g.op(g.call('SUM', 'z.q.x'), '>', g.call('SUM', 'z.s.x')))
    result['c14_direct_control'] = g.select([g.alias(g.call('SUM', 'k'), 'a'), g.alias(g.call('MAX', 'k'), 'b')], 'l')
    assert set(result) == set(CASES)
    return result


def metadata(case, n):
    bug = CASES[case]
    flags = []
    if bug in ('C09', 'C10', 'C11'):
        flags = ['DisableAnsiInForEmptyOrNullableItemsCollections' if 'legacy' in case
                 else 'AnsiInForEmptyOrNullableItemsCollections']
    return {'bug': bug, 'deterministic': True, 'compare_legacy_hash': True,
            'repetitions': 1, 'pragmas': [], 'flag_pragmas': flags,
            'table_service_overrides': {},
            'assertions': ['Successful execution', 'Match legacy unordered result hash',
                           'check_rows returns semantic_oracle_pass'],
            'integration_note': ('Fallback-sensitive legacy tests additionally require C11'
                                 if bug == 'C09' else 'No native verification claimed')}


def check_rows(case, rows, n):
    try:
        if case in ('c09_ansi_null_rhs', 'c09_legacy_null_lookup_empty', 'c10_null_in'):
            valid = not rows
        elif case.startswith(('c09_', 'c10_', 'c11_')):
            valid = len(rows) == 1 and rows[0]['id'] == 1
        elif case == 'c12_hidden_group_key':
            valid = len(rows) == 1 and rows[0]['c'] == 1
        elif case == 'c12_visible_group_key':
            valid = sorted((r['id'], r['c']) for r in rows) == [(i, 1) for i in range(1, n + 1)]
        elif case == 'c12_no_distinct':
            valid = len(rows) == n and all(r['c'] == 1 for r in rows)
        elif case == 'c13_distinct_group_expression':
            valid = sorted(r['v'] for r in rows) == list(range(11, n + 11))
        elif case.startswith('c13_'):
            expected = {'c13_aggregate_group_expression': n + 1,
                        'c13_direct_group_control': 1, 'c13_plain_group_expression': 11}[case]
            valid = len(rows) == 1 and rows[0]['v'] == expected
        elif case.startswith('c14_'):
            expected = (3, 3) if case == 'c14_direct_control' else (1, 2)
            valid = len(rows) == 1 and (rows[0]['a'], rows[0]['b']) == expected
        else:
            raise KeyError('Unknown expression check label')
    except (KeyError, TypeError, ValueError):
        return {'semantic_oracle_pass': False, 'result_shape_valid': False}
    return {'semantic_oracle_pass': bool(valid), 'result_shape_valid': True}
