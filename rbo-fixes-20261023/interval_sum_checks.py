"""F02 compositional interval SUM checks; never persist rendered queries or rows."""
from collections import Counter
import hashlib
import itertools
import json

import generate_and_verify as g

GENERATOR_VERSION = 'f02_exact_interval_sum_v1'
_LIMITS = {'Interval': 86400000000 * 49673 - 1,
           'Interval64': 4611669811199999999 - (-4611669897600000000)}


def specs():
    result = {}
    for typ, scenario, grouped, blocks in itertools.product(
            _LIMITS, ('balanced', 'positive_overflow', 'negative_overflow', 'cancellation',
                     'range_edge', 'all_null', 'empty'), (False, True), (False, True)):
        case = 'f02_' + '_'.join((typ.lower(), scenario, 'grouped' if grouped else 'scalar',
                                 'blocks' if blocks else 'rows'))
        result[case] = dict(typ=typ, scenario=scenario, grouped=grouped, blocks=blocks,
                            optional=scenario == 'all_null', distinct=False, multi=False, nodes=1)
    for typ, grouped, multi, optional in itertools.product(_LIMITS, (False, True), (False, True), (False, True)):
        case = 'f02_' + '_'.join((typ.lower(), 'optional' if optional else 'required', 'multi_distinct' if multi else 'ordinary',
                                 'grouped' if grouped else 'scalar'))
        result[case] = dict(typ=typ, scenario='cancellation', grouped=grouped, blocks=False,
                            optional=optional, distinct=multi, multi=multi, nodes=1)
    for typ, grouped, optional in itertools.product(_LIMITS, (False, True), (False, True)):
        case = 'f02_' + '_'.join((typ.lower(), 'optional' if optional else 'required', 'single_distinct',
                                 'grouped' if grouped else 'scalar'))
        result[case] = dict(typ=typ, scenario='cancellation', grouped=grouped, blocks=False,
                            optional=optional, distinct=True, multi=False, nodes=1)
    for typ, scenario in itertools.product(_LIMITS, ('balanced', 'cancellation', 'all_null')):
        case = 'f02_' + '_'.join((typ.lower(), scenario, 'four_nodes'))
        result[case] = dict(typ=typ, scenario=scenario, grouped=True, blocks=False,
                            optional=scenario == 'all_null', distinct=False, multi=False, nodes=4)
    for typ in ('Int64', 'Decimal(35,0)'):
        case = 'f02_' + ('integer' if typ == 'Int64' else 'decimal') + '_control'
        result[case] = dict(typ=typ, scenario='balanced', grouped=True, blocks=False,
                            optional=False, distinct=False, multi=False, nodes=1)
    return result


def _size(n):
    if not isinstance(n, int) or not 8 <= n <= 4096:
        raise ValueError('Generated size must be between 8 and 4096')


def _pattern(spec):
    limit = _LIMITS.get(spec['typ'], 10**6)
    scenario = spec['scenario']
    if scenario == 'positive_overflow':
        return (limit // 2 + 1, limit // 2 + 1, limit // 2 + 1, limit // 2 + 1)
    if scenario == 'negative_overflow':
        return (-limit // 2 - 1,) * 4
    if scenario == 'cancellation':
        return (limit - 7, limit - 11, -limit + 7, -limit + 11)
    if scenario == 'range_edge':
        return (limit, 0, -limit, 1)
    return (-101, 19, 103, -17)


def _integer(value):
    return g.typed('Int64', value)


def _mod(left, value):
    return g.call('Unwrap', g.op(left, '%', _integer(value)))


def _cast(value, typ):
    return 'CAST(' + value + ' AS ' + typ + ')'


def _source(spec):
    pattern = _pattern(spec)
    bucket = _mod(g.op('id', '-', _integer(1)), len(pattern))
    value = _integer(pattern[-1])
    for index in reversed(range(len(pattern) - 1)):
        value = g.call('IF', g.op(bucket, '=', _integer(index)), _integer(pattern[index]), value)
    if spec['typ'] != 'Int64':
        value = g.call('Unwrap', _cast(value, spec['typ']))
    if spec['scenario'] == 'all_null':
        value = g.call('Nothing', spec['typ'] + '?')
    elif spec['optional']:
        value = g.call('IF', g.op(_mod('id', 5), '=', _integer(0)),
                       g.call('Nothing', spec['typ'] + '?'), g.call('Just', value))
    return g.derived(g.select(['id', g.alias(_mod('id', 3), 'p'), g.alias(value, 'v')], 't',
                             where=g.op('id', '<', _integer(0)) if spec['scenario'] == 'empty' else None), 's')


def recipes(n):
    _size(n)
    result = {}
    for case, spec in specs().items():
        argument = 'DISTINCT v' if spec['distinct'] else 'v'
        fields = [g.alias(_cast(g.call('SUM', argument), 'Int64'), 's')]
        if spec['multi']:
            fields += [g.alias(_cast(g.call('SUM', 'v'), 'Int64'), 'a'),
                       g.alias(g.call('COUNT', 'DISTINCT id'), 'c')]
        if spec['grouped']:
            fields.insert(0, 'p')
        result[case] = g.select(fields, _source(spec), group='p' if spec['grouped'] else None)
    return result


def _finish_sum(values, typ, distinct=False):
    present = [value for value in values if value is not None]
    if not present:
        return None
    total = sum(set(present) if distinct else present)
    limit = _LIMITS.get(typ)
    if limit is not None and not -limit <= total <= limit:
        return None
    return total


def expected_rows(case, n):
    _size(n)
    spec = specs()[case]
    ids = [] if spec['scenario'] == 'empty' else list(range(1, n + 1))
    groups = sorted({i % 3 for i in ids}) if spec['grouped'] else [None]
    pattern = _pattern(spec)
    result = []
    for group in groups:
        members = [i for i in ids if group is None or i % 3 == group]
        values = [None if spec['scenario'] == 'all_null' or spec['optional'] and i % 5 == 0
                  else pattern[(i - 1) % len(pattern)] for i in members]
        row = {'p': group} if spec['grouped'] else {}
        row['s'] = _finish_sum(values, spec['typ'], spec['distinct'])
        if spec['multi']:
            row.update(a=_finish_sum(values, spec['typ']), c=len(members))
        result.append(row)
    return result


def check_rows(case, rows, n):
    canonical = lambda items: Counter(json.dumps(item, sort_keys=True, separators=(',', ':')) for item in items)
    return canonical(rows) == canonical(expected_rows(case, n))


semantic_oracle = check_rows


def requirements(case, mode='new'):
    if mode not in ('new', 'legacy'):
        raise ValueError('Unknown optimizer mode')
    spec = specs()[case]
    return {'generator_version': GENERATOR_VERSION, 'node_count': spec['nodes'],
            'peak_native_concurrency': 1,
            'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                                     'DqHashOperatorsUseBlocks': spec['blocks'],
                                     'EnableNewRBOPhysicalStagePeephole': False,
                                     'EnableQueryServiceSpilling': True,
                                     'EnableSpillingInHashJoinShuffleConnections': True,
                                     'EnableSpillingNodes': 'All'},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable')],
            'result_order_required': False,
            'expected_lowering': 'Decimal interval accumulation before state splitting and final interval conversion'}


def metadata(case, n, mode='new'):
    spec = specs()[case]
    _size(n)
    return dict(requirements(case, mode), finding='F02',
                acceptance_scope='ordinary and DISTINCT aggregate interval SUM; bounded window path excluded',
                result_rows=len(expected_rows(case, n)),
                distributed_placement_requires_native_evidence=spec['nodes'] > 1)


def schema_and_seeds(n):
    _size(n)
    return g.schema_and_seeds(n, GENERATOR_VERSION)


def artifact_contracts():
    counts = Counter()
    for n in (8, 24, 53):
        generated = recipes(n)
        for case in specs():
            rows = expected_rows(case, n)
            assert generated[case] and check_rows(case, rows, n)
            counts['builder_oracle_acceptances'] += 1
            mutated = [dict(row) for row in rows]
            if mutated:
                mutated[0]['s'] = 1 if mutated[0]['s'] is None else mutated[0]['s'] + 1
            else:
                mutated = [{'s': 0}]
            assert not check_rows(case, mutated, n)
            counts['mutated_result_rejections'] += 1
            if rows:
                assert not check_rows(case, rows + rows[:1], n)
                counts['duplicate_result_rejections'] += 1
    for typ, limit in _LIMITS.items():
        for optional, distinct in itertools.product((False, True), repeat=2):
            for length in range(1, 5):
                for values in itertools.product((-limit, -1, 0, 1, limit), repeat=length):
                    source = list(values) + ([None] if optional else [])
                    expected = _finish_sum(source, typ, distinct)
                    present = [value for value in source if value is not None]
                    domain = sorted(set(present)) if distinct else present
                    # Decimal(35,0) is exact for the bounded model sizes.
                    for cut in range(len(domain) + 1):
                        merged = sum(domain[:cut]) + sum(domain[cut:])
                        actual = merged if -limit <= merged <= limit else None
                        assert actual == expected
                        counts['split_merge_laws'] += 1
        for sign in (-1, 1):
            sequence = [sign * limit, sign * limit, -sign * limit]
            bounded = None
            for value in sequence:
                total = value if bounded is None else bounded + value
                bounded = total if -limit <= total <= limit else None
            assert bounded != _finish_sum(sequence, typ)
            counts['premature_interval_overflow_mutations_rejected'] += 1
    return dict(counts)


if __name__ == '__main__':
    print(json.dumps({'case_count': len(specs()), 'contracts': artifact_contracts(),
                      'generator_sha256': hashlib.sha256(open(__file__, 'rb').read()).hexdigest()}, sort_keys=True))
