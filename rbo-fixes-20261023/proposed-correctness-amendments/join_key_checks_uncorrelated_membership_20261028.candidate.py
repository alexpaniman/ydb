"""Compositional C50 checks with independent bag oracles and four-node settings.

Rendered queries, fixtures and result rows belong only in caller memory/anonymous
files. This module stores builders and models; it never runs a native command.
"""
import importlib.util
import json
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location('window_builders_for_join_checks', _HERE / 'window_checks.py')
w = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(w)
g = w.g
CASE_SPECIFIC_SCHEMA = True


def _kind(label):
    for token, kind in [('right_semi', 'RIGHT SEMI'), ('right_anti', 'RIGHT ONLY'),
                        ('left_semi', 'LEFT SEMI'), ('left_anti', 'LEFT ONLY'),
                        ('full', 'FULL'), ('right', 'RIGHT'), ('left', 'LEFT')]:
        if token in label:
            return kind
    return 'INNER'


def _rows(n, label, side):
    nulls = 'nullable' in label or 'nullsafe' in label
    rows = []
    for i in range(1, n + 1):
        key = i % 11 - 5 if side == 'left' else (i * 3) % 13 - 6
        if nulls and i % (7 if side == 'left' else 5) == 0:
            key = None
        rows.append((i, key))
    if 'empty_' + side in label:
        return []
    return rows


def schema_and_seeds(n, case):
    w._size(n)
    ddl = []
    seeds = []
    for side, table, typ in [('left', 'jl', 'Int64'), ('right', 'jr', 'Double')]:
        columns = ['id Int64 NOT NULL', 'k ' + typ, 'PRIMARY KEY (id)']
        ddl.append('CREATE TABLE ' + table + ' (' + ', '.join(columns) + ')')
        # Keep the physical table populated even for an empty-side predicate.
        values = _rows(n, case.replace('empty_', 'filtered_'), side)
        arms = [g.select([g.alias(g.typed('Int64', i), 'id'),
                          g.alias(g.call('Nothing', typ + '?') if key is None else g.call('Just', g.typed(typ, key)), 'k')])
                for i, key in values]
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(arms))
    ddl.append('CREATE TABLE jt (id Int64 NOT NULL, PRIMARY KEY (id))')
    seeds.append('UPSERT INTO jt ' + g.combine([g.select([g.alias(g.typed('Int64', i), 'id')]) for i in range(1, n + 1)]))
    return '; '.join(ddl) + ';', seeds


def _input(label, side):
    key = 'k'
    if side == 'left' and 'normalized' in label:
        key = w._cast(key, 'Double')
    elif side == 'left' and 'int32' in label:
        key = w._cast(key, 'Int32')
    elif side == 'right' and 'float' in label:
        key = w._cast(key, 'Float')
    elif side == 'right' and 'same_type' in label:
        key = w._cast(key, 'Int64')
    if 'uncorrelated' in label:
        # These bounded fixtures contain no NULL key values. COALESCE makes
        # the lookup/result annotations required, so NOT IN can use the
        # simple anti-join route without nullable IN's separate contract.
        typ = 'Int64' if side == 'left' or 'same_type' in label else 'Double'
        key = g.call('COALESCE', key, g.typed(typ, 0))
    fields = ['id', g.alias(key, 'k'), g.alias(w._mod('id', 3), 'p')]
    if 'repeated_keys' in label and side == 'right':
        fields.append(g.alias(w._cast('k', 'Float'), 'k2'))
    return g.derived(g.select(fields, 'jl' if side == 'left' else 'jr',
                              where=g.op('id', '<', w._integer(0)) if 'empty_' + side in label else None),
                     'a' if side == 'left' else 'b')


def _query(label):
    kind = _kind(label)
    equal = 'IS NOT DISTINCT FROM' if 'nullsafe' in label else '='
    predicate = g.op('a.k', equal, 'b.k')
    if 'composite' in label:
        predicate = g.op(predicate, 'AND', g.op('a.p', '=', 'b.p'))
    if 'repeated_keys' in label:
        predicate = g.op(predicate, 'AND', g.op('a.k', '=', 'b.k2'))
    if kind in ('LEFT SEMI', 'LEFT ONLY', 'RIGHT SEMI', 'RIGHT ONLY'):
        # Explicit SEMI/ONLY join syntax is rejected by the new SQL parser.
        # Correlated existence supplies the admitted semantic route to these
        # physical join families; keep same-type controls for decorrelation.
        left_output = kind.startswith('LEFT ')
        if 'uncorrelated' in label:
            outer_side, inner_side = ('left', 'right') if left_output else ('right', 'left')
            outer_key, inner_key = ('a.k', 'b.k') if left_output else ('b.k', 'a.k')
            inner = g.select([inner_key], _input(label, inner_side))
            condition = g.op(outer_key, 'NOT IN' if kind.endswith('ONLY') else 'IN', '(' + inner + ')')
            return g.select([g.alias('a.id' if left_output else 'b.id', 'li' if left_output else 'ri')],
                            _input(label, outer_side), where=condition)
        outer_side, inner_side = ('left', 'right') if left_output else ('right', 'left')
        inner = g.select([w._integer(1)], _input(label, inner_side), where=predicate)
        condition = g.call('EXISTS', inner)
        if kind.endswith('ONLY'):
            condition = 'NOT ' + condition
        return g.select([g.alias('a.id' if left_output else 'b.id', 'li' if left_output else 'ri')],
                        _input(label, outer_side), where=condition)
    fields = []
    if not kind.startswith('RIGHT ') or kind == 'RIGHT':
        fields.append(g.alias('a.id', 'li'))
    if not kind.startswith('LEFT ') or kind == 'LEFT':
        fields.append(g.alias('b.id', 'ri'))
    query = g.select(fields, g.join(_input(label, 'left'), _input(label, 'right'), predicate, kind))
    if 'grouped_after_join' in label:
        query = g.select(['li', g.alias(g.call('COUNT', '*'), 'c')], g.derived(query, 'q'), group='li')
    elif 'join_chain' in label:
        query = g.select([g.alias('q.li', 'li'), g.alias('q.ri', 'ri'), g.alias('c.id', 'ti')],
                         g.join(g.derived(query, 'q'), 'jt AS c', g.op('q.li', '=', 'c.id')))
    return query


def recipes(n):
    w._size(n)
    labels = [
        'c50_int_double_inner', 'c50_int_double_left', 'c50_int_double_right', 'c50_int_double_full',
        'c50_left_semi', 'c50_left_anti', 'c50_right_semi', 'c50_right_anti',
        'c50_left_semi_same_type_control', 'c50_left_anti_same_type_control',
        'c50_right_semi_same_type_control', 'c50_right_anti_same_type_control',
        'c50_left_semi_uncorrelated', 'c50_left_anti_uncorrelated',
        'c50_right_semi_uncorrelated', 'c50_right_anti_uncorrelated',
        'c50_left_semi_uncorrelated_same_type_control', 'c50_left_anti_uncorrelated_same_type_control',
        'c50_right_semi_uncorrelated_same_type_control', 'c50_right_anti_uncorrelated_same_type_control',
        'c50_nullable_inner', 'c50_nullable_left', 'c50_nullable_full', 'c50_nullsafe_inner',
        'c50_int32_double_inner', 'c50_int_float_inner', 'c50_normalized_control', 'c50_same_type_control',
        'c50_nullable_normalized_control', 'c50_composite_keys', 'c50_repeated_keys',
        'c50_grouped_after_join', 'c50_grouped_after_join_se', 'c50_join_chain', 'c50_join_chain_se',
        'c50_int_double_inner_row', 'c50_int_double_inner_single_node',
        'c50_full_empty_left', 'c50_full_empty_right',
    ]
    return {label: _query(label) for label in labels}


def requirements(label, mode='new'):
    if mode not in ('new', 'legacy'):
        raise ValueError('Unknown optimizer mode')
    elimination = label.endswith('_se')
    return {
        'generator_version': ('required_membership_controls_v4' if 'uncorrelated' in label
                              else 'uniform_optional_fixture_exists_v3'),
        'bug': 'C50', 'node_count': 1 if 'single_node' in label else 4,
        'minimum_rows': 8, 'recommended_rows': 48, 'repetitions': 2,
        'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable'),
                    ('kikimr.CostBasedOptimizationLevel', '2' if elimination else '0'),
                    ('kikimr.HashJoinMode', 'grace'),
                    ('kikimr.OptShuffleElimination', 'true' if elimination else 'false')],
        'table_service_config': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                                 'UseBlockHashJoin': not label.endswith('_row'),
                                 'DqHashOperatorsUseBlocks': False,
                                 'DefaultEnableShuffleElimination': elimination,
                                 'EnableQueryServiceSpilling': True,
                                 'EnableSpillingInHashJoinShuffleConnections': True,
                                 'EnableSpillingNodes': 'All'},
        'expected_lowering': 'Mixed typed keys use two plain UnionAll inputs to one regular join task; homogeneous controls may remain distributed.',
        'plan_acceptance': ['Both gather inputs are nonparallel UnionAll.',
                            'Join task count equals one for mixed-key joins.',
                            'No stale CBO shuffle-elimination hint is reused after a gather.',
                            'Gathered output is not advertised as hash partitioned.'],
        'setup_note': 'Use anonymous generated schema/fixtures/config/results. Legacy is a secondary comparison; the bag oracle is authoritative.',
    }


def expected_rows(label, n):
    left, right = _rows(n, label, 'left'), _rows(n, label, 'right')
    nullsafe = 'nullsafe' in label
    def matches(l, r):
        if l[1] is None or r[1] is None:
            equal = nullsafe and l[1] is None and r[1] is None
        else:
            equal = l[1] == r[1]
        return equal and ('composite' not in label or l[0] % 3 == r[0] % 3)
    pairs = [(l[0], r[0]) for l in left for r in right if matches(l, r)]
    matched_left = {l for l, _ in pairs}
    matched_right = {r for _, r in pairs}
    kind = _kind(label)
    if kind in ('LEFT SEMI', 'LEFT ONLY'):
        expected = [{'li': i} for i, _ in left if (i in matched_left) == (kind == 'LEFT SEMI')]
    elif kind in ('RIGHT SEMI', 'RIGHT ONLY'):
        expected = [{'ri': i} for i, _ in right if (i in matched_right) == (kind == 'RIGHT SEMI')]
    else:
        expected = [{'li': l, 'ri': r} for l, r in pairs]
        if kind in ('LEFT', 'FULL'):
            expected += [{'li': i, 'ri': None} for i, _ in left if i not in matched_left]
        if kind in ('RIGHT', 'FULL'):
            expected += [{'li': None, 'ri': i} for i, _ in right if i not in matched_right]
    if 'grouped_after_join' in label:
        counts = {}
        for row in expected:
            counts[row['li']] = counts.get(row['li'], 0) + 1
        return [{'li': key, 'c': value} for key, value in sorted(counts.items())]
    if 'join_chain' in label:
        return [dict(row, ti=row['li']) for row in expected]
    return expected


def semantic_oracle(label, rows, n):
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values)
    return canonical(rows) == canonical(expected_rows(label, n))


check_rows = semantic_oracle


def paired_controls():
    result = {'c50_int_double_inner': ['c50_normalized_control', 'c50_same_type_control',
                                      'c50_int_double_inner_single_node', 'c50_int_double_inner_row'],
            'c50_nullable_inner': ['c50_nullable_normalized_control'],
            'c50_grouped_after_join_se': ['c50_grouped_after_join'],
            'c50_join_chain_se': ['c50_join_chain']}
    for direction in ('left', 'right'):
        for kind in ('semi', 'anti'):
            label = 'c50_' + direction + '_' + kind + '_uncorrelated'
            result[label] = [label + '_same_type_control']
    return result
