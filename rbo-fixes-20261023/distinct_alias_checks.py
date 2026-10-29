"""F01 compositional alias checks; generated SQL and rows are never persisted."""
import json

import generate_and_verify as g

GENERATOR_VERSION = 'f01_distinct_input_aliases_v1'


def specs():
    return {'f01_' + '_'.join((shape, typ, 'empty' if empty else 'rows', 'blocks' if blocks else 'scalar')):
            dict(shape=shape, typ=typ, empty=empty, blocks=blocks)
            for shape in ('repeat_two', 'repeat_three', 'mixed_expression', 'different_inputs')
            for typ in ('required', 'optional') for empty in (False, True) for blocks in (False, True)}


def recipes(n):
    result = {}
    for case, spec in specs().items():
        bucket = g.call('Unwrap', g.op('id', '%', g.typed('Int64', 3)))
        value = g.call('IF', g.op(bucket, '=', g.typed('Int64', 0)), g.call('Nothing', 'Int64?'),
                       g.call('Just', bucket)) if spec['typ'] == 'optional' else bucket
        source = g.derived(g.select([g.alias(value, 'g'), g.alias(bucket, 'h')], 't',
                                   where='id < 0' if spec['empty'] else None), 's')
        expressions = ['g', 'g']
        if spec['shape'] == 'repeat_three':
            expressions.append('g')
        elif spec['shape'] == 'mixed_expression':
            expressions = ['g', g.op('g', '+', g.typed('Int64', 1)), 'g']
        elif spec['shape'] == 'different_inputs':
            expressions = ['g', 'h']
        fields = [g.alias(expression, chr(ord('a') + index)) for index, expression in enumerate(expressions)]
        result[case] = g.select(fields, source, distinct=True)
    return result


def expected_rows(case, n):
    spec = specs()[case]
    values = set()
    for i in ([] if spec['empty'] else range(1, n + 1)):
        bucket = i % 3
        value = None if spec['typ'] == 'optional' and bucket == 0 else bucket
        row = (value, value)
        if spec['shape'] == 'repeat_three':
            row += (value,)
        elif spec['shape'] == 'mixed_expression':
            row = (value, None if value is None else value + 1, value)
        elif spec['shape'] == 'different_inputs':
            row = (value, bucket)
        values.add(row)
    return [{chr(ord('a') + index): value for index, value in enumerate(row)} for row in values]


def check_rows(case, rows, n):
    canonical = lambda items: sorted(json.dumps(item, sort_keys=True) for item in items)
    return canonical(rows) == canonical(expected_rows(case, n))


def requirements(case, mode='new'):
    return {'generator_version': GENERATOR_VERSION,
            'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                                     'DqHashOperatorsUseBlocks': specs()[case]['blocks'],
                                     'EnableNewRBOPhysicalStagePeephole': False},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable')]}
