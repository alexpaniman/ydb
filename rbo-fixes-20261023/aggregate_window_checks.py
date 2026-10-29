"""Compositional checks for C24/C27/C28/C35; never persist rendered inputs.

The caller owns anonymous input/config/result files. This module only renders
recipes in memory and compares parsed rows with independent Python models.
No native command is launched here. Use at least 8 rows, preferably 24.
"""
import importlib.util
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location('rbo_window_builders', _HERE / 'window_checks.py')
w = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(w)
g = w.g


def _average_source(n, label):
    if 'numeric' in label:
        value = 'id'
    elif 'decimal' in label:
        value = w._cast('id', 'Decimal(18,2)', required=True)
    else:
        value = w._interval_expression(n, extended='interval64' in label,
                                       optional='optional' in label,
                                       all_null='all_null' in label)
    if ('numeric' in label or 'decimal' in label) and 'optional' in label:
        typ = 'Int64' if 'numeric' in label else 'Decimal(18,2)'
        value = g.call('IF', g.op(w._mod('id', 4), '=', w._integer(0)),
                       g.call('Nothing', typ + '?'), g.call('Just', value))
    return g.select(['id', g.alias(w._mod('id', 3), 'p'), g.alias(value, 'v')], 't',
                    where=g.op('id', '<', w._integer(0)) if 'empty' in label else None)


def _average(n, label):
    grouped = 'grouped' in label
    multi = 'multi_distinct' in label
    argument = 'DISTINCT v' if multi else 'v'
    fields = [g.alias(w._cast(g.call('AVG', argument), 'Int64'), 'a'),
              g.alias(g.call('COUNT', 'DISTINCT id' if multi else 'v'), 'c')]
    if grouped:
        fields.insert(0, 'p')
    return g.select(fields, g.derived(_average_source(n, label), 's'), group='p' if grouped else None)


def _sum_source(n, label):
    value = g.op(w._mod('id', 5), '-', w._integer(2))
    typ = 'Int64' if 'numeric' in label else ('Interval64' if 'interval64' in label else 'Interval')
    if typ != 'Int64':
        value = w._cast(g.op(value, '*', w._integer(1000003)), typ, required=True)
    if 'all_null' in label:
        value = g.call('Nothing', typ + '?')
    elif 'optional' in label or 'nullable' in label:
        value = g.call('IF', g.op(w._mod('id', 4), '=', w._integer(0)),
                       g.call('Nothing', typ + '?'), g.call('Just', value))
    return g.select(['id', g.alias(w._mod('id', 3), 'p'), g.alias(value, 'v')], 't',
                    where=g.op('id', '<', w._integer(0)) if 'empty' in label else None)


def _sum(n, label):
    grouped = 'grouped' in label
    fields = [g.alias(w._cast(g.call('SUM', 'DISTINCT v'), 'Int64'), 'd'),
              g.alias(w._cast(g.call('SUM', 'v'), 'Int64'), 's'),
              g.alias(g.call('COUNT', 'DISTINCT id'), 'c')]
    if grouped:
        fields.insert(0, 'p')
    return g.select(fields, g.derived(_sum_source(n, label), 's'), group='p' if grouped else None)


def _distinct_window(label):
    source = g.derived(w._source(), 's')
    rank = g.over(g.call('RANK'), 'p', 'v ASC')
    fields = ['p', g.alias(rank, 'r')]
    if label == 'c28_row_number_distinct':
        fields = ['p', g.alias(g.over(g.call('ROW_NUMBER'), 'p', 'id ASC'), 'r')]
    elif label == 'c28_window_expression_distinct':
        fields = ['p', g.alias(g.op(rank, '+', g.typed('Uint64', 7)), 'r')]
    elif label == 'c28_multiple_windows':
        fields.append(g.alias(g.over(g.call('DENSE_RANK'), 'p', 'v DESC'), 'd'))
    elif label == 'c28_plain_distinct_control':
        fields = ['p', g.alias('v', 'r')]
    return g.select(fields, source, distinct=label != 'c28_window_nondistinct_control', order='p, r')


def _having(label):
    source = g.derived(g.select(['id', g.alias(w._mod('id', 3), 'p')], 't',
                                where=g.op('id', '<', w._integer(0)) if 'empty' in label else None), 's')
    predicate = g.op('p', '>', w._integer(0))
    if 'constant_true' in label:
        predicate = g.typed('Bool', 'true')
    elif 'constant_false' in label:
        predicate = g.typed('Bool', 'false')
    elif 'nested' in label:
        predicate = g.op(g.op('p', '+', w._integer(1)), '>', w._integer(1))
    elif 'aggregate_control' in label:
        predicate = g.op(g.call('COUNT', '*'), '>', g.typed('Uint64', 0))
    return g.select(['p', g.alias(g.call('COUNT', '*'), 'c')], source, group='p', having=predicate)


def recipes(n):
    w._size(n)
    labels = [
        'c35_interval_scalar', 'c35_interval_grouped', 'c35_interval_optional_scalar',
        'c35_interval_optional_grouped', 'c35_interval64_precise_scalar',
        'c35_interval64_optional_grouped', 'c35_interval_empty',
        'c35_interval_all_null', 'c35_interval_empty_grouped',
        'c35_interval_multi_distinct', 'c35_interval_optional_multi_distinct',
        'c35_numeric_control', 'c35_decimal_control',
        'c24_interval_scalar', 'c24_interval_grouped', 'c24_interval_optional',
        'c24_interval_optional_grouped', 'c24_interval_all_null', 'c24_interval_empty',
        'c24_interval_empty_grouped', 'c24_interval64_control',
        'c24_numeric_control', 'c24_nullable_numeric_control',
        'c24_avg_numeric_multi_distinct_control', 'c24_avg_optional_numeric_multi_distinct_control',
        'c24_avg_decimal_multi_distinct_control', 'c24_avg_optional_decimal_multi_distinct_control',
        'c28_rank_distinct', 'c28_row_number_distinct', 'c28_window_expression_distinct',
        'c28_multiple_windows', 'c28_window_nondistinct_control', 'c28_plain_distinct_control',
        'c27_group_key', 'c27_constant_true', 'c27_constant_false',
        'c27_nested_group_expression', 'c27_aggregate_control', 'c27_empty',
    ]
    result = {}
    for label in labels:
        result[label] = (_average(n, label) if label.startswith(('c35_', 'c24_avg_')) else
                         _sum(n, label) if label.startswith('c24_') else
                         _distinct_window(label) if label.startswith('c28_') else _having(label))
    for label, query in w.recipes(n).items():
        if label.startswith('c35_window_'):
            result[label] = query
    return result


def schema_and_seeds(n):
    w._size(n)
    return g.schema_and_seeds(n, 'aggregate_window_checks')


def requirements(label, mode='new'):
    if mode not in ('new', 'legacy'):
        raise ValueError('Unknown optimizer mode')
    result = w.requirements(label, mode)
    result['generator_version'] = 'explicit_nonblock_interval_rounding_v3'
    result['table_service_config'].update(DqHashOperatorsUseBlocks=False,
                                           EnableQueryServiceSpilling=True,
                                           EnableSpillingInHashJoinShuffleConnections=True,
                                           EnableSpillingNodes='All')
    result['expected_lowering'] = ('ordinary AVG and its split/final states' if label.startswith('c35_') else
                                   'multi-DISTINCT branch expansion' if label.startswith('c24_') else
                                   'result DISTINCT above windows' if label.startswith('c28_') else
                                   'grouped HAVING filter')
    result['acceptance_scope'] = 'independent bug branch'
    result['result_order_required'] = False
    result['block_engine'] = False
    result['setup_note'] = 'Use matching server mode, ordinary seeds, blocks disabled and anonymous input/config/result files.'
    return result


def _mean(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    total = sum(values)
    return (-1 if total < 0 else 1) * (abs(total) // len(values))


def expected_rows(label, n):
    """Pure model; generated expectations may be held only in caller memory."""
    w._size(n)
    if label.startswith('c35_window_'):
        return None  # The existing window semantic oracle handles row attachments.
    if label.startswith(('c24_', 'c35_')):
        grouped = 'grouped' in label
        ids = [] if 'empty' in label else list(range(1, n + 1))
        groups = sorted(set(i % 3 for i in ids)) if grouped else [None]
        expected = []
        for p in groups:
            members = [i for i in ids if p is None or i % 3 == p]
            values = []
            for i in members:
                if label.startswith(('c35_', 'c24_avg_')):
                    value = (i if 'numeric' in label or 'decimal' in label else
                             2 ** 53 + i * 13 - (n + 3) if 'interval64' in label else
                             (i % 7 - 3) * 1000003 + i)
                else:
                    value = (i % 5 - 2) * (1 if 'numeric' in label else 1000003)
                if 'all_null' in label or ('optional' in label or 'nullable' in label) and i % 4 == 0:
                    value = None
                values.append(value)
            present = [v for v in values if v is not None]
            row = {'p': p} if grouped else {}
            if label.startswith(('c35_', 'c24_avg_')):
                multi = 'multi_distinct' in label
                mean = _mean if 'numeric' in label or 'decimal' in label else w._interval_mean
                row.update(a=mean(list(set(present)) if multi else present),
                           c=len(members) if multi else len(present))
            else:
                row.update(d=sum(set(present)) if present else None,
                           s=sum(present) if present else None, c=len(members))
            expected.append(row)
        return expected
    if label.startswith('c27_'):
        if 'empty' in label or 'constant_false' in label:
            return []
        groups = range(3) if 'constant_true' in label or 'aggregate_control' in label else (1, 2)
        return [{'p': p, 'c': sum(i % 3 == p for i in range(1, n + 1))} for p in groups]
    if label.startswith('c28_'):
        expected = []
        for p in range(3):
            ids = [i for i in range(1, n + 1) if i % 3 == p]
            distinct = sorted(set(i % 4 for i in ids))
            for pos, i in enumerate(ids, 1):
                value = i % 4
                rank = 1 + sum(j % 4 < value for j in ids)
                if label == 'c28_row_number_distinct':
                    rank = pos
                elif label == 'c28_window_expression_distinct':
                    rank += 7
                elif label == 'c28_plain_distinct_control':
                    rank = value
                row = {'p': p, 'r': rank}
                if label == 'c28_multiple_windows':
                    row['d'] = 1 + sum(v > value for v in distinct)
                if label == 'c28_window_nondistinct_control' or row not in expected:
                    expected.append(row)
        return expected
    raise KeyError(label)


def semantic_oracle(label, parsed_rows, n):
    if label.startswith('c35_window_'):
        return w.semantic_oracle(label, parsed_rows, n)
    expected = expected_rows(label, n)
    # Preserve duplicates for nondistinct controls; object key order is irrelevant.
    canonical = lambda rows: sorted(tuple(sorted(row.items())) for row in rows)
    return canonical(parsed_rows) == canonical(expected)


check_rows = semantic_oracle


def paired_controls():
    return {
        'c35_interval_scalar': ['c35_numeric_control', 'c35_decimal_control'],
        'c35_interval_optional_grouped': ['c35_interval_grouped', 'c35_interval_all_null'],
        'c35_interval_multi_distinct': ['c35_interval_scalar'],
        'c24_interval_scalar': ['c24_numeric_control', 'c24_nullable_numeric_control'],
        'c24_interval_optional': ['c24_interval_scalar', 'c24_interval_all_null'],
        'c28_rank_distinct': ['c28_window_nondistinct_control', 'c28_plain_distinct_control'],
        'c27_group_key': ['c27_aggregate_control', 'c27_constant_true', 'c27_constant_false'],
    }
