"""Four-node distribution checks with independent closed-form oracles.

Only compositional builders and scalar formulas persist. Rendered inputs,
results, and plans belong to the anonymous native harness buffers.
"""
import json

import generate_and_verify as g


def recipes(n):
    if not isinstance(n, int) or n < 2:
        raise ValueError('Distribution checks require at least two rows')
    original = g.recipes(n)
    count = g.call('COUNT', '*')
    single = g.select(['id'], 't', where=g.op('id', '=', '1'))
    result = {
        'c37_window_distribution': original['window_distribution'],
        'c37_shuffle_disabled_control': original['window_distribution'],
        'c38_full_distribution': original['full_distribution'],
        'c38_shuffle_disabled_control': original['full_distribution'],
    }
    ranked = g.select([
        g.alias('a.k', 'k'),
        g.alias(g.over(g.call('ROW_NUMBER'), 'a.k', 'b.id'), 'r')],
        g.join('t AS a', 't AS b', g.op('a.k', '=', 'b.k')))
    result['c37_aligned_partition_control'] = g.select(
        ['k', g.alias(g.call('SUM', 'r'), 'n')], g.derived(ranked, 's'), group='k')
    unpartitioned = g.select([
        g.alias('a.k', 'k'),
        g.alias(g.over(g.call('ROW_NUMBER'), order='b.id'), 'r')],
        g.join('t AS a', 't AS b', g.op('a.k', '=', 'b.k')))
    result['c37_keyless_window_control'] = g.select(
        ['k', g.alias(g.call('SUM', 'r'), 'n')], g.derived(unpartitioned, 's'), group='k')
    first_window = g.select([
        g.alias('a.id', 'a_id'), g.alias('b.id', 'b_id'), g.alias('a.k', 'k'),
        g.alias(g.over(g.call('ROW_NUMBER'), 'a.id', 'b.id'), 'r')],
        g.join('t AS a', 't AS b', g.op('a.k', '=', 'b.k')))
    second_window = g.select([
        'k', 'r', g.alias(g.over(g.call('ROW_NUMBER'), 'k', 'a_id, b_id'), 's')],
        g.derived(first_window, 'w'))
    result['c37_consecutive_windows'] = g.select([
        'k', g.alias(g.call('SUM', 'r'), 'n'), g.alias(g.call('SUM', 's'), 'm')],
        g.derived(second_window, 'z'), group='k')
    for kind in ('INNER', 'LEFT', 'RIGHT'):
        result['c38_' + kind.lower() + '_distribution'] = g.select(
            ['a.id', g.alias(count, 'n')],
            g.join(g.derived(single, 'a'), 't AS b', g.op('a.id', '=', 'b.id'), kind),
            group='a.id')
    result['c38_right_key_distribution'] = g.select(
        ['b.id', g.alias(count, 'n')],
        g.join('t AS a', g.derived(single, 'b'), g.op('a.id', '=', 'b.id'), 'FULL'),
        group='b.id')
    return result


def unavailable_recipes(n):
    # The new SQL translator rejects EXCLUSION before optimizer import. Keep
    # its compositional builder for future operator-level/translator coverage,
    # but do not count a parser rejection as distribution-fix verification.
    single = g.select(['id'], 't', where=g.op('id', '=', '1'))
    return {'c38_exclusion_distribution': g.select(
        ['a.id', g.alias(g.call('COUNT', '*'), 'n')],
        g.join(g.derived(single, 'a'), 't AS b', g.op('a.id', '=', 'b.id'), 'EXCLUSION'),
        group='a.id')}


def schema_and_seeds(n):
    return g.schema_and_seeds(n, 'distribution')


def metadata(case, n):
    return {
        'generator_version': 'distribution-independent-oracles-v1',
        'node_count': 4,
        'pragmas': [('kikimr.HashJoinMode', 'grace')],
        'table_service_overrides': {
            'DefaultEnableShuffleElimination': 'shuffle_disabled' not in case},
        'assertions': ['Independent expected multiset', 'New and legacy agree'],
    }


def expected_rows(case, n):
    if case.startswith('c37_'):
        # The self join has n^2 rows. Partitioning by one input row gives n
        # partitions with ranks 1..n; partitioning by the shared constant key
        # gives one partition with ranks 1..n^2.
        total = n * n * (n + 1) // 2
        if case in ('c37_aligned_partition_control', 'c37_keyless_window_control'):
            total = n * n * (n * n + 1) // 2
        expected = [{'k': 1, 'n': total}]
        if case == 'c37_consecutive_windows':
            expected[0]['m'] = n * n * (n * n + 1) // 2
    elif case == 'c38_exclusion_distribution':
        expected = [{'id': None, 'n': n - 1}]
    elif case in ('c38_left_distribution', 'c38_inner_distribution'):
        expected = [{'id': 1, 'n': 1}]
    else:
        expected = [{'id': 1, 'n': 1}, {'id': None, 'n': n - 1}]
    if case.startswith('c38_'):
        name = 'b.id' if case == 'c38_right_key_distribution' else 'a.id'
        expected = [{name: row['id'], 'n': row['n']} for row in expected]
    return expected


def check_rows(case, rows, n):
    expected = expected_rows(case, n)
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True) for row in values)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected),
            'independent_multiset_oracle': True}
