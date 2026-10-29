"""Separate C23 DISTINCT boundary controls; original failing profiles stay intact."""
import json

import generate_and_verify as g

GENERATOR_VERSION = 'c23_distinct_boundary_v1'


def specs():
    return {'c23_distinct_boundary_' + '_'.join((shape, 'empty' if empty else 'rows', 'blocks' if blocks else 'scalar')):
            dict(shape=shape, empty=empty, blocks=blocks)
            for shape in ('plain_null', 'paired_null', 'alias_ordinary_control', 'alias_optional_control')
            for empty in (False, True) for blocks in (False, True)}


def recipes(n):
    result = {}
    for case, spec in specs().items():
        shape = spec['shape']
        value = ('k' if shape == 'alias_ordinary_control' else
                 g.call('Nothing', 'Int64?') if shape == 'alias_optional_control' else 'NULL')
        source = g.derived(g.select([g.alias(value, 'g'), g.alias('NULL', 'h')], 't',
                                   where='id < 0' if spec['empty'] else None), 's')
        fields = [g.alias('g', 'a'), g.alias('g', 'b')] if shape.startswith('alias_') else (
            ['g', 'h'] if shape == 'paired_null' else ['g'])
        result[case] = g.select(fields, source, distinct=True)
    return result


def expected_rows(case, n):
    spec = specs()[case]
    if spec['empty'] or n == 0:
        return []
    if spec['shape'].startswith('alias_'):
        value = 1 if spec['shape'] == 'alias_ordinary_control' else None
        return [{'a': value, 'b': value}]
    return [{'g': None, 'h': None}] if spec['shape'] == 'paired_null' else [{'g': None}]


def check_rows(case, rows, n):
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True) for row in values)
    return canonical(rows) == canonical(expected_rows(case, n))


def requirements(case, mode='new'):
    return {'generator_version': GENERATOR_VERSION,
            'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                                     'DqHashOperatorsUseBlocks': specs()[case]['blocks'],
                                     'EnableNewRBOPhysicalStagePeephole': False},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable')]}
