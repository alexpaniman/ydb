"""C28 result-boundary builders and row oracles; never persist rendered inputs."""
import importlib.util
import json
from collections import Counter
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location('c28_window_builders', Path(__file__).with_name('window_checks.py'))
w = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(w)
g = w.g
GENERATOR_VERSION = 'c28_window_result_projection_v1'


def specs():
    return {'c28_projection_' + shape + ('_empty' if empty else '_rows'):
            {'shape': shape, 'empty': empty}
            for shape in ('hidden_partition', 'computed_output', 'shadow_output', 'repeat_alias',
                          'computed_partition', 'computed_order', 'count_frame', 'grouped_visible',
                          'grouped_hidden', 'ordered_limit', 'scalar_window_expression', 'input_alias', 'constant_output')
            for empty in (False, True)}


def recipes(n):
    w._size(n)
    result = {}
    for case, spec in specs().items():
        shape = spec['shape']
        source = g.derived(g.select(['id', g.alias(w._mod('id', 3), 'p'), g.alias(w._mod('id', 4), 'v')],
                                   't', where=g.op('id', '<', w._integer(0)) if spec['empty'] else None), 's')
        partition = g.op('p', '+', w._integer(1)) if shape == 'computed_partition' else 'p'
        order = g.op('v', '+', w._mod('id', 2)) if shape == 'computed_order' else 'v ASC'
        rank = g.over(g.call('RANK'), partition, order)
        fields, grouping, ordering, limit = ['p', g.alias(rank, 'r')], None, None, None
        if shape == 'hidden_partition':
            fields = [g.alias(rank, 'r')]
        elif shape == 'constant_output':
            fields = ['p', g.alias(w._integer(5), 'x'), g.alias(rank, 'r')]
        elif shape == 'computed_output':
            fields = ['p', g.alias(w._mod('v', 2), 'x'), g.alias(rank, 'r')]
        elif shape == 'shadow_output':
            fields = ['p', g.alias(g.op('v', '+', w._integer(1)), 'v'), g.alias(rank, 'r')]
        elif shape == 'repeat_alias':
            fields = [g.alias('p', 'a'), g.alias('p', 'b'), g.alias(rank, 'r')]
        elif shape == 'count_frame':
            fields = ['p', g.alias(g.over(g.call('COUNT', '*'), 'p', 'id ASC',
                                         'ROWS BETWEEN 2 PRECEDING AND CURRENT ROW'), 'c')]
        elif shape == 'grouped_visible':
            count = g.call('COUNT', '*')
            fields = ['p', g.alias(count, 'c'), g.alias(g.over(g.call('RANK'), 'p', count), 'r')]
            grouping = 'p, v'
        elif shape == 'grouped_hidden':
            fields = [g.alias(g.over(g.call('RANK'), None, 'p ASC'), 'r')]
            grouping = 'p, v'
        elif shape == 'ordered_limit':
            ordering, limit = 'p, r', 4
        elif shape == 'scalar_window_expression':
            fields = ['p', g.alias(g.op(rank, '+', g.typed('Uint64', 7)), 'r')]
        elif shape == 'input_alias':
            fields = [g.alias('v', 'value'), g.alias(rank, 'r')]
        result[case] = g.select(fields, source, distinct=True, group=grouping, order=ordering, limit=limit)
    return result


def expected_rows(case, n):
    w._size(n)
    spec = specs()[case]
    if spec['empty']:
        return []
    shape = spec['shape']
    rows = [{'id': i, 'p': i % 3, 'v': i % 4} for i in range(1, n + 1)]
    if shape.startswith('grouped_'):
        groups = Counter((row['p'], row['v']) for row in rows)
        rows = [{'p': p, 'v': v, 'c': count} for (p, v), count in groups.items()]
    output = []
    for row in rows:
        if shape == 'grouped_hidden':
            result = {'r': 1 + sum(other['p'] < row['p'] for other in rows)}
        else:
            peers = [other for other in rows if other['p'] == row['p']]
            key = (lambda item: item['c']) if shape == 'grouped_visible' else (
                  (lambda item: item['v'] + item['id'] % 2) if shape == 'computed_order' else
                  (lambda item: item['v']))
            rank = 1 + sum(key(other) < key(row) for other in peers)
            result = {'p': row['p'], 'r': rank}
            if shape == 'hidden_partition':
                result = {'r': rank}
            elif shape == 'constant_output':
                result['x'] = 5
            elif shape == 'computed_output':
                result['x'] = row['v'] % 2
            elif shape == 'shadow_output':
                result['v'] = row['v'] + 1
            elif shape == 'repeat_alias':
                result = {'a': row['p'], 'b': row['p'], 'r': rank}
            elif shape == 'count_frame':
                result = {'p': row['p'], 'c': min(3, 1 + sum(other['id'] < row['id'] for other in peers))}
            elif shape == 'grouped_visible':
                result['c'] = row['c']
            elif shape == 'scalar_window_expression':
                result['r'] += 7
            elif shape == 'input_alias':
                result = {'value': row['v'], 'r': rank}
        if result not in output:
            output.append(result)
    if shape == 'ordered_limit':
        output = sorted(output, key=lambda row: (row['p'], row['r']))[:4]
    return output


def check_rows(case, rows, n):
    canonical = lambda items: sorted(json.dumps(item, sort_keys=True) for item in items)
    expected = expected_rows(case, n)
    if specs()[case]['shape'] == 'ordered_limit':
        return rows == expected
    return canonical(rows) == canonical(expected)


def requirements(case, mode='new'):
    spec = specs()[case]
    result = w.requirements(case, mode)
    result['generator_version'] = GENERATOR_VERSION
    result['table_service_config'].update(DqHashOperatorsUseBlocks=False,
                                          EnableNewRBOPhysicalStagePeephole=False)
    result['result_order_required'] = spec['shape'] == 'ordered_limit'
    result['acceptance_scope'] = 'C28 independent window result DISTINCT branch'
    result['expected_lowering'] = 'Selected projection and DISTINCT follow all window operators.'
    return result
