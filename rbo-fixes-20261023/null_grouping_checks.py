"""Compositional C23 contracts; generated statements and rows stay in RAM."""
import json

import generate_and_verify as g

GENERATOR_VERSION = 'c23_null_physical_hash_keys_v1'


def specs():
    shapes = ('sets', 'grouped', 'mixed', 'distinct', 'distinct_mixed',
              'null_argument', 'multiple_nulls', 'indicator', 'hidden',
              'typed_optional', 'ordinary')
    return {'c23_' + '_'.join((shape, 'empty' if empty else 'rows', 'blocks' if blocks else 'scalar')):
            dict(shape=shape, empty=empty, blocks=blocks)
            for shape in shapes for empty in (False, True) for blocks in (False, True)}


def _query(spec):
    shape = spec['shape']
    key = g.call('Nothing', 'Int64?') if shape == 'typed_optional' else ('k' if shape == 'ordinary' else 'NULL')
    bucket = g.call('Unwrap', g.op('id', '%', g.typed('Int64', 3)))
    source = g.derived(g.select([g.alias(key, 'g'), g.alias('NULL', 'h'),
                                 g.alias(bucket, 'q'), 'id'], 't',
                                where='id < 0' if spec['empty'] else None), 's')
    count = g.alias(g.call('COUNT', '*'), 'c')
    if shape == 'distinct':
        return g.select([g.alias('g', 'a'), g.alias('g', 'b')], source, distinct=True)
    if shape == 'distinct_mixed':
        return g.select(['g', 'q'], source, distinct=True)
    if shape == 'null_argument':
        return g.select(['g', g.alias(g.call('COUNT', 'g'), 'c'),
                         g.alias(g.call('SUM', 'id'), 's')], source, group='g')
    if shape == 'grouped':
        return g.select(['g', count], source, group='g')
    if shape == 'mixed':
        return g.select(['g', 'q', count], source, group='GROUPING SETS ((g, q), (g), ())')
    if shape == 'multiple_nulls':
        return g.select(['g', 'h', count], source, group='GROUPING SETS ((g, h), (g), ())')
    fields = [count] if shape == 'hidden' else ['g', count]
    if shape == 'indicator':
        fields.append(g.alias(g.call('GROUPING', 'g'), 'm'))
    return g.select(fields, source, group='GROUPING SETS ((g), ())')


def recipes(n):
    return {case: _query(spec) for case, spec in specs().items()}


def expected_rows(case, n):
    spec = specs()[case]
    shape = spec['shape']
    ids = [] if spec['empty'] else list(range(1, n + 1))
    size = len(ids)
    buckets = sorted({i % 3 for i in ids})
    if shape == 'distinct':
        return [{'a': None, 'b': None}] if ids else []
    if shape == 'distinct_mixed':
        return [{'g': None, 'q': bucket} for bucket in buckets]
    if shape == 'null_argument':
        return [{'g': None, 'c': 0, 's': sum(ids)}] if ids else []
    if shape == 'grouped':
        return [{'g': None, 'c': size}] if ids else []
    if shape == 'mixed':
        result = [{'g': None, 'q': bucket, 'c': sum(i % 3 == bucket for i in ids)} for bucket in buckets]
        if ids:
            result.append({'g': None, 'q': None, 'c': size})
        return result + [{'g': None, 'q': None, 'c': size}]
    if shape == 'multiple_nulls':
        return [{'g': None, 'h': None, 'c': size} for _ in range(3 if ids else 1)]
    if shape == 'hidden':
        return [{'c': size} for _ in range(2 if ids else 1)]
    if shape == 'indicator':
        return ([{'g': None, 'c': size, 'm': 0}] if ids else []) + [{'g': None, 'c': size, 'm': 1}]
    return ([{'g': 1 if shape == 'ordinary' else None, 'c': size}] if ids else []) + [{'g': None, 'c': size}]


def check_rows(case, rows, n):
    canonical = lambda items: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in items)
    return canonical(rows) == canonical(expected_rows(case, n))


def requirements(case, mode='new'):
    spec = specs()[case]
    return {'generator_version': GENERATOR_VERSION,
            'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                                     'DqHashOperatorsUseBlocks': spec['blocks'],
                                     'EnableNewRBOPhysicalStagePeephole': False},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable')],
            'node_count': 2 if spec['shape'] == 'mixed' else 1}
