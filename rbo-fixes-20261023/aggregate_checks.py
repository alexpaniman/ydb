"""Compositional aggregate checks; rendered inputs and results stay in memory."""
import math
import generate_and_verify as g


def _mod(value, divisor):
    return g.call('Unwrap', g.op(value, '%', g.typed('Int64', divisor)))


def recipes(n):
    base = g.recipes(n)
    result = {'c03_distinct_rollup': base['distinct_rollup'],
              'c26_distinct_variance': base['distinct_variance'],
              'c34_tuple_some': base['tuple_some']}
    for empty in (False, True):
        source = g.derived(g.select([g.alias(_mod('id', 3), 'g'),
                                    g.alias(_mod('id', 5), 'v')], 't',
                                   where='id < 0' if empty else None), 's')
        for rollup in (False, True):
            label = 'c03_' + ('rollup' if rollup else 'grouped_control') + ('_empty' if empty else '_nonempty')
            result[label] = g.select(['g', g.alias(g.call('COUNT', 'DISTINCT v'), 'c')], source,
                                     group=g.call('ROLLUP', 'g') if rollup else 'g')
    count = g.call('COUNT', '*')
    nulls = g.derived(g.select([g.alias('NULL', 'g'), 'id'], 't'), 's')
    result['c23_bare_null_grouping'] = g.select(['g', g.alias(count, 'c')], nulls,
                                              group='GROUPING SETS ((g), ())')
    nulls_empty = g.derived(g.select([g.alias('NULL', 'g'), 'id'], 't', where='id < 0'), 's')
    result['c23_bare_null_empty'] = g.select(['g', g.alias(count, 'c')], nulls_empty,
                                           group='GROUPING SETS ((g), ())')
    for name, expression in [('nullable', g.call('Just', 'k')), ('ordinary', 'k')]:
        source = g.derived(g.select([g.alias(expression, 'g'), 'id'], 't'), 's')
        result['c23_' + name + '_control'] = g.select(['g', g.alias(count, 'c')], source,
                                                     group='GROUPING SETS ((g), ())')
    total = g.op(g.call('COUNT', 'DISTINCT b.k'), '+', g.call('SUM', 'DISTINCT b.id'))
    inner = g.select([total], 't AS b', where=g.op('b.id', '=', 'a.id'))
    result['c25_correlated_multi_distinct'] = g.select(['a.id', g.alias('(' + inner + ')', 'v')], 't AS a')
    result['c25_uncorrelated_control'] = g.select([g.alias(total, 'v')], 't AS b')
    empty_inner = g.select([total], 't AS b', where=g.op('b.id', '=', g.op('a.id', '+', g.typed('Int64', n))))
    result['c25_correlated_empty'] = g.select(['a.id', g.alias('(' + empty_inner + ')', 'v')], 't AS a')
    for variant in ('optional', 'all_null', 'empty', 'single', 'grouped', 'mixed_avg'):
        expression = g.call('Nothing', 'Int64?') if variant == 'all_null' else (
            g.call('IF', g.op('id', '%', '3') + ' = 0', g.call('Nothing', 'Int64?'), g.call('Just', 'k'))
            if variant == 'optional' else 'k')
        source = g.derived(g.select(['id', g.alias(expression, 'v'), 'k'], 't',
                                   where='id < 0' if variant == 'empty' else ('id = 1' if variant == 'single' else None)), 's')
        fields = [g.alias(g.call('COUNT', 'DISTINCT v'), 'c'), g.alias(g.call('STDDEV_SAMP', 'v'), 's')]
        if variant == 'mixed_avg':
            fields.append(g.alias(g.call('AVG', 'v'), 'a'))
        if variant == 'grouped':
            fields.append('k')
        result['c26_' + variant] = g.select(fields, source, group='k' if variant == 'grouped' else None)
    # variance_1_1 is the variance-family aggregate admitted by this RBO.
    for function in ('STDDEV_SAMP',):
        for optional in (False, True):
            value = (g.call('IF', g.op(_mod('id', 3), '=', g.typed('Int64', 0)),
                            g.call('Nothing', 'Int64?'), g.call('Just', 'id')) if optional else 'id')
            source = g.derived(g.select(['id', g.alias(value, 'v')], 't'), 's')
            fields = [g.alias(g.call('COUNT', 'DISTINCT v'), 'c'), g.alias(g.call(function, 'v'), 's')]
            label = 'c26_varying_' + function.lower() + ('_optional' if optional else '')
            result[label] = g.select(fields, source)
    result['c26_variance_control'] = g.select([g.alias(g.call('STDDEV_SAMP', 'id'), 's')], 't')
    for function in ('MIN', 'MAX', 'SOME'):
        for empty in (False, True):
            fields = [g.alias(g.call(function, g.call('AsTuple', 'id', 'k', g.op('id', '+', '1'))), 'v')]
            result['c34_' + function.lower() + ('_empty' if empty else '_control')] = g.select(fields, 't', where='id < 0' if empty else None)
        fields.append(g.alias(g.call('AVG', 'k'), 'a'))
        result['c34_' + function.lower() + '_mixed_avg'] = g.select(fields, 't')
    return result


def check_rows(case, rows, n):
    if case == 'c03_distinct_rollup':
        return sorted((r['g'] is not None, r['g'] or 0, r['c']) for r in rows) == [(False, 0, 1), (True, 1, 1), (True, 2, 1)]
    if case.startswith('c03_'):
        ids = [] if case.endswith('_empty') else list(range(1, n + 1))
        expected = [{'g': key, 'c': len({i % 5 for i in ids if i % 3 == key})}
                    for key in sorted({i % 3 for i in ids})]
        if 'rollup' in case:
            expected.append({'g': None, 'c': len({i % 5 for i in ids})})
        canonical = lambda values: sorted((str(row['g']), row['c']) for row in values)
        return canonical(rows) == canonical(expected)
    if case.startswith('c23_'):
        if case == 'c23_bare_null_empty':
            return rows == [{'g': None, 'c': 0}]
        expected = [None, None] if case == 'c23_bare_null_grouping' else [None, 1]
        return len(rows) == 2 and sorted(str(r['g']) for r in rows) == sorted(map(str, expected)) and all(r['c'] == n for r in rows)
    if case == 'c25_correlated_multi_distinct':
        return sorted((r['id'], r['v']) for r in rows) == [(i, i + 1) for i in range(1, n + 1)]
    if case == 'c25_uncorrelated_control':
        return rows == [{'v': 1 + n * (n + 1) // 2}]
    if case == 'c25_correlated_empty':
        return sorted((r['id'], r['v']) for r in rows) == [(i, None) for i in range(1, n + 1)]
    if case.startswith('c26_'):
        expected_columns = ({'s'} if case == 'c26_variance_control' else
                            {'k', 'c', 's'} if case == 'c26_grouped' else
                            {'c', 's', 'a'} if case == 'c26_mixed_avg' else {'c', 's'})
        if len(rows) != 1 or set(rows[0]) != expected_columns:
            return False
    if case.startswith('c26_varying_') or case == 'c26_variance_control':
        values = [i for i in range(1, n + 1) if not case.endswith('_optional') or i % 3]
        mean = sum(values) / len(values)
        value = sum((i - mean) ** 2 for i in values) / (len(values) - (0 if '_pop' in case else 1))
        if 'stddev' in case or case == 'c26_variance_control':
            value = math.sqrt(value)
        return (len(rows) == 1 and math.isclose(rows[0]['s'], value, rel_tol=1e-10, abs_tol=1e-10)
                and (case == 'c26_variance_control' or rows[0]['c'] == len(values)))
    if case.startswith('c26_'):
        if case == 'c26_grouped' and rows[0]['k'] != 1:
            return False
        absent = case in ('c26_all_null', 'c26_empty')
        if case == 'c26_single':
            value = rows[0]['s']
            nan = isinstance(value, float) and math.isnan(value) or isinstance(value, str) and value.lower() == 'nan'
            return rows[0]['c'] == 1 and nan
        return rows[0]['c'] == (0 if absent else 1) and rows[0]['s'] == (None if absent else 0) and (case != 'c26_mixed_avg' or rows[0]['a'] == 1)
    if case.startswith('c34_'):
        expected_columns = {'v', 'a'} if case.endswith('_mixed_avg') else {'v'}
        if len(rows) != 1 or set(rows[0]) != expected_columns:
            return False
        value = rows[0]['v']
        if case.endswith('_empty'):
            return value is None
        expected_ids = [1] if case.startswith('c34_min_') else [n] if case.startswith('c34_max_') else range(1, n + 1)
        return isinstance(value, list) and len(value) == 3 and value[0] in expected_ids and value[1] == 1 and value[2] == value[0] + 1 and (not case.endswith('_mixed_avg') or rows[0]['a'] == 1)
    raise KeyError(case)


def requirements(case, mode='new'):
    return {'generator_version': 'aggregate_oracle_v4',
            'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable')]}
