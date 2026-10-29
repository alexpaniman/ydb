"""F03 compositional interval-window contracts; rendered inputs stay in memory."""
from collections import Counter
import hashlib
import itertools
import json

import generate_and_verify as g

GENERATOR_VERSION = 'f03_interval_window_decimal_state_v1'
LIMITS = {'Interval': 86400000000 * 49673 - 1,
          'Interval64': 4611669811199999999 - (-4611669897600000000)}
FRAMES = {'rows_overlap': ('ROWS', -2, 1), 'rows_running': ('ROWS', None, 0),
          'rows_suffix': ('ROWS', 0, None), 'rows_prior': ('ROWS', -4, -1),
          'rows_following': ('ROWS', 1, 3), 'rows_incremental': ('ROWS', None, -2),
          'range_overlap': ('RANGE', -2, 1), 'range_running': ('RANGE', None, 0),
          'range_suffix': ('RANGE', 0, None), 'range_incremental': ('RANGE', None, -2)}


def specs():
    result = {}
    def add(typ, scenario, frame, optional=False, descending=False):
        case = 'f03_' + '_'.join((typ.lower(), scenario, frame,
                                 'optional' if optional else 'required', 'desc' if descending else 'asc'))
        result[case] = dict(typ=typ, scenario=scenario, frame=frame,
                            optional=optional, descending=descending)
    for typ, scenario, frame in itertools.product(LIMITS,
            ('balanced', 'cancellation', 'positive_overflow', 'all_null'),
            ('rows_overlap', 'rows_running', 'rows_suffix', 'range_overlap')):
        add(typ, scenario, frame)
    for typ, frame, optional in itertools.product(LIMITS,
            ('rows_prior', 'rows_following', 'rows_incremental', 'range_running',
             'range_suffix', 'range_incremental'), (False, True)):
        add(typ, 'cancellation', frame, optional, descending=optional)
    for typ in LIMITS:
        add(typ, 'empty', 'rows_overlap')
        add(typ, 'negative_overflow', 'rows_running')
        add(typ, 'negative_overflow', 'range_overlap', descending=True)
        add(typ, 'range_edge', 'rows_overlap', optional=True)
        add(typ, 'balanced', 'range_peers', optional=True)
    for typ in ('Int64', 'Decimal(35,0)'):
        add(typ, 'balanced', 'rows_overlap', optional=True)
    return result


def _integer(value):
    return g.typed('Int64', value)


def _mod(value, divisor):
    return g.call('Unwrap', g.op(value, '%', _integer(divisor)))


def _cast(value, typ):
    return 'CAST(' + value + ' AS ' + typ + ')'


def _pattern(spec):
    limit = LIMITS.get(spec['typ'], 1000000)
    if spec['scenario'] == 'cancellation':
        return (limit - 7, limit - 11, -limit + 7, -limit + 11)
    if spec['scenario'] == 'positive_overflow':
        return (limit // 2 + 1,) * 4
    if spec['scenario'] == 'negative_overflow':
        return (-limit // 2 - 1,) * 4
    if spec['scenario'] == 'range_edge':
        return (limit, 0, -limit, 1)
    return (-101, 19, 103, -17)


def _bound(value, lower):
    if value is None:
        return 'UNBOUNDED PRECEDING' if lower else 'UNBOUNDED FOLLOWING'
    return 'CURRENT ROW' if value == 0 else str(abs(value)) + (' PRECEDING' if value < 0 else ' FOLLOWING')


def _query(spec):
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
    peer_case = spec['frame'] == 'range_peers'
    source = g.select(['id', g.alias(_mod('id', 3), 'p'), g.alias(_mod('id', 2 if peer_case else 7), 'o'),
                       g.alias(_mod('id', 4 if peer_case else 5), 'q'), g.alias(value, 'v')], 't',
                      where=g.op('id', '<', _integer(0)) if spec['scenario'] == 'empty' else None)
    kind, lower, upper = FRAMES.get(spec['frame'], ('RANGE', None, 0))
    frame = kind + ' BETWEEN ' + _bound(lower, True) + ' AND ' + _bound(upper, False)
    direction = ' DESC' if spec['descending'] else ' ASC'
    order = 'o ASC, q ASC' if spec['frame'] == 'range_peers' else ('o' if kind == 'RANGE' else 'id') + direction
    fields = ['id', g.alias(g.over(g.call('SUM', 'v'), 'p', order, frame), 's'),
              g.alias(g.over(g.call('COUNT', 'v'), 'p', order, frame), 'c'),
              g.alias(g.over(g.call('COUNT', 'id'), 'p', order, frame), 'f')]
    return g.select(['id', g.alias(_cast('s', 'Int64'), 's'), 'c', 'f'],
                    g.derived(g.select(fields, g.derived(source, 'w')), 'r'), order='id')


def recipes(n):
    if type(n) is not int or not 8 <= n <= 4096:
        raise ValueError('Generated size must be between 8 and 4096')
    return {case: _query(spec) for case, spec in specs().items()}


def _finish(values, typ):
    present = [value for value in values if value is not None]
    if not present:
        return None
    total = sum(present)
    limit = LIMITS.get(typ)
    return total if limit is None or -limit <= total <= limit else None


def expected_rows(case, n):
    spec = specs()[case]
    if spec['scenario'] == 'empty':
        return []
    pattern = _pattern(spec)
    peer_case = spec['frame'] == 'range_peers'
    source = [dict(id=i, p=i % 3, o=i % (2 if peer_case else 7), q=i % (4 if peer_case else 5),
                   v=None if spec['scenario'] == 'all_null' or spec['optional'] and i % 5 == 0
                   else pattern[(i - 1) % len(pattern)]) for i in range(1, n + 1)]
    kind, lower, upper = FRAMES.get(spec['frame'], ('RANGE', None, 0))
    result = []
    for row in source:
        partition = [item for item in source if item['p'] == row['p']]
        if spec['frame'] == 'range_peers':
            members = [item for item in partition if (item['o'], item['q']) <= (row['o'], row['q'])]
        elif kind == 'RANGE':
            sign = -1 if spec['descending'] else 1
            members = [item for item in partition if
                       (lower is None or sign * (item['o'] - row['o']) >= lower) and
                       (upper is None or sign * (item['o'] - row['o']) <= upper)]
        else:
            partition.sort(key=lambda item: item['id'], reverse=spec['descending'])
            position = next(i for i, item in enumerate(partition) if item['id'] == row['id'])
            members = [item for index, item in enumerate(partition) if
                       (lower is None or index - position >= lower) and
                       (upper is None or index - position <= upper)]
        values = [item['v'] for item in members]
        result.append(dict(id=row['id'], s=_finish(values, spec['typ']),
                           c=sum(value is not None for value in values), f=len(members)))
    return result


def check_rows(case, rows, n):
    canonical = lambda data: Counter(json.dumps(item, sort_keys=True, separators=(',', ':')) for item in data)
    return canonical(rows) == canonical(expected_rows(case, n))


semantic_oracle = check_rows


def requirements(case, mode='new'):
    return {'generator_version': GENERATOR_VERSION,
            'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                                     'DqHashOperatorsUseBlocks': False,
                                     'EnableNewRBOPhysicalStagePeephole': False},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable'),
                        ('ydb.WindowFunctionsV2', 'true')],
            'node_count': 1, 'peak_native_concurrency': 1}


def metadata(case, n, mode='new'):
    return dict(requirements(case, mode), finding='F03',
                acceptance_scope='Physical-window interval SUM; logical whole-partition aggregate expansion is separate F02',
                result_rows=len(expected_rows(case, n)),
                native_plan_reachability_pending=True)


def artifact_contracts():
    counts = Counter()
    for n in (8, 24, 53):
        rendered = recipes(n)
        for case in specs():
            expected = expected_rows(case, n)
            assert rendered[case] and check_rows(case, expected, n)
            counts['builder_oracle_acceptances'] += 1
            changed = [dict(row) for row in expected]
            if changed:
                changed[0]['s'] = 1 if changed[0]['s'] is None else changed[0]['s'] + 1
                assert not check_rows(case, changed, n)
                assert not check_rows(case, expected + expected[:1], n)
            else:
                assert not check_rows(case, [dict(id=1, s=None, c=0, f=0)], n)
            counts['oracle_mutation_rejections'] += 1
    peer_counts = Counter((i % 3, i % 2, i % 4) for i in range(1, 25))
    assert all(count >= 2 for count in peer_counts.values())
    assert any(len({q for p, o, q in peer_counts if p == partition and o == first}) > 1
               for partition in range(3) for first in range(2))
    counts['duplicate_composite_peer_contracts'] += 2
    for typ, limit in LIMITS.items():
        for length in range(1, 6):
            for values in itertools.product((None, -limit, -1, 0, 1, limit), repeat=length):
                # Model the shared optional Decimal accumulator and independent
                # final conversion without narrowing intermediate states.
                state = None
                for value in values:
                    if value is not None:
                        state = value if state is None else state + value
                actual = None if state is None or not -limit <= state <= limit else state
                assert actual == _finish(values, typ)
                counts['optional_wide_state_laws'] += 1
        for values in ((limit, limit, -limit), (-limit, -limit, limit)):
            narrowed = None
            for value in values:
                narrowed = value if narrowed is None else narrowed + value
                if not -limit <= narrowed <= limit:
                    narrowed = None
            assert narrowed != _finish(values, typ)
            counts['premature_overflow_mutations_rejected'] += 1
    return dict(counts)


if __name__ == '__main__':
    print(json.dumps({'case_count': len(specs()), 'contracts': artifact_contracts(),
                      'generator_sha256': hashlib.sha256(open(__file__, 'rb').read()).hexdigest()}, sort_keys=True))
