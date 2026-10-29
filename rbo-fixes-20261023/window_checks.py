"""Compositional verification recipes for independent window/ordering fixes.

Only builders and semantic models are stored. The caller must keep generated
schema, seed statements, queries and result payloads in anonymous memory and
persist only verdicts/hashes/categories. This module never runs a native tool.

API: recipes(n), schema_and_seeds(n), requirements(label, mode),
paired_controls(), semantic_oracle(label, parsed_rows, n).
Pass rows in their returned order: C36 checks ordering as well as contents.
Use n >= 8; n >= 24 provides more ties and multiple rows per partition.
All C21 acceptance cases have explicit non-whole ROWS frames. Whole-partition
expansion probes are labeled C35 and are not acceptance tests for the C21 branch.
"""
import importlib.util
import math
from pathlib import Path

_REVIEW = Path(__file__).resolve().parents[1] / 'rbo-review-20261021'
_SPEC = importlib.util.spec_from_file_location('rbo_review_builders', _REVIEW / 'generate_and_verify.py')
g = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(g)

_RUNNING = 'ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW'
_PREVIOUS = 'ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING'
_GROUPS = 3


def _size(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('Window verification requires at least eight generated rows')
    return n


def _integer(value):
    return g.typed('Int64', value)


def _cast(value, typ, required=False):
    result = g.call('CAST', str(value) + ' AS ' + typ)
    return g.call('Unwrap', result) if required else result


def _mod(value, modulus):
    # Integer remainder has an Optional result type even with a known nonzero
    # divisor. Remove that layer so nullable rank inputs have exactly one layer.
    return g.call('Unwrap', g.op(value, '%', _integer(modulus)))


def _source(extra=(), nullable=False):
    value = _mod('id', 4)
    if nullable:
        value = g.call('IF', g.op(_mod('id', 5), '=', _integer(0)),
                       g.call('Nothing', 'Int64?'), g.call('Just', value))
    fields = ['id', g.alias(_mod('id', _GROUPS), 'p'), g.alias(value, 'v')]
    return g.select(fields + list(extra), 't')


def _ranking(source, order=None, row_number=False, partition='p'):
    fields = ['id', 'p', 'v', g.alias(g.over(g.call('RANK'), partition, order), 'r'),
              g.alias(g.over(g.call('DENSE_RANK'), partition, order), 'd')]
    if row_number:
        fields.append(g.alias(g.over(g.call('ROW_NUMBER'), partition, order), 'rn'))
    return g.select(fields, g.derived(source, 'q'), order='id')


def _interval_expression(n, extended=False, optional=False, all_null=False):
    typ = 'Interval64' if extended else 'Interval'
    if all_null:
        return g.call('Nothing', typ + '?')
    if extended:
        micros = g.op(g.op(_integer(2 ** 53), '+', g.op('id', '*', _integer(13))), '-', _integer(n + 3))
    else:
        micros = g.op(g.op(g.op(_mod('id', 7), '-', _integer(3)), '*', _integer(1000003)), '+', 'id')
    value = _cast(micros, typ, required=True)
    if optional:
        value = g.call('IF', g.op(_mod('id', 4), '=', _integer(0)),
                       g.call('Nothing', typ + '?'), g.call('Just', value))
    return value


def _interval_window(n, *, extended=False, optional=False, all_null=False, frame=None):
    source = g.select(['id', g.alias(_mod('id', _GROUPS), 'p'),
                       g.alias(_interval_expression(n, extended, optional, all_null), 'v')], 't')
    order = 'id' if frame else None
    mean = g.over(g.call('AVG', 'v'), 'p', order, frame)
    return g.select(['id', 'p', g.alias(_cast(mean, 'Int64'), 'a')],
                    g.derived(source, 'q'), order='id')


def _nested_source(n):
    fields = [g.alias(g.op(g.op('id', '*', _integer(3)), '+', _integer(1)), 'value'),
              g.alias(_mod('id', _GROUPS), 'bucket'),
              g.alias(g.op(_integer(n), '-', 'id'), 'position')]
    return g.select(['id', g.alias(_mod('id', _GROUPS), 'p'),
                     g.alias(g.call('AsStruct', *fields), 'box'),
                     g.alias(g.op('id', '+', _integer(1000)), 'value'),
                     g.alias(_integer(0), 'bucket'), g.alias('id', 'position')], 't')


def _nested_window(n, purpose, control=False):
    source = _nested_source(n)
    if control:
        source = g.select(['id', 'p', g.alias('q.box.value', 'actual_value'),
                           g.alias('q.box.bucket', 'actual_bucket'),
                           g.alias('q.box.position', 'actual_position')], g.derived(source, 'q'))
    ref = lambda leaf: ('q.actual_' if control else 'q.box.') + leaf
    argument = ref('value') if purpose == 'argument' else 'q.id'
    partition = ref('bucket') if purpose == 'partition' else ('p' if purpose == 'order' else None)
    order = ref('position') if purpose == 'order' else ('id' if purpose == 'partition' else None)
    value = g.over(g.call('SUM', argument), partition, order, _RUNNING if order else None)
    return g.select(['id', g.alias(value, 'a')], g.derived(source, 'q'), order='id')


def recipes(n):
    """Render requested families only in the caller's process memory."""
    _size(n)
    source = _source()
    result = {
        'c19_unordered': _ranking(source, row_number=True),
        'c19_ordered_control': _ranking(source, 'v ASC', row_number=True),
    }
    for mode in ('ansi', 'legacy'):
        for direction in ('asc', 'desc'):
            result[f'c20_{mode}_{direction}'] = _ranking(_source(nullable=True), 'v ' + direction.upper())
        nulls = g.select(['id', g.alias(_mod('id', _GROUPS), 'p'),
                          g.alias(g.call('Nothing', 'Int64?'), 'v')], 't')
        result[f'c20_{mode}_all_null'] = _ranking(nulls, 'v ASC')
        result[f'c20_{mode}_nonnullable_control'] = _ranking(source, 'v ASC')

    result.update({
        'c21_interval_running': _interval_window(n, frame=_RUNNING),
        'c21_interval_optional_running': _interval_window(n, optional=True, frame=_RUNNING),
        'c21_interval_empty_frame': _interval_window(n, optional=True, frame=_PREVIOUS),
        'c21_interval_all_null_running': _interval_window(n, all_null=True, frame=_RUNNING),
        'c21_interval64_precise_running': _interval_window(n, extended=True, frame=_RUNNING),
        'c21_interval64_optional_running': _interval_window(n, extended=True, optional=True, frame=_RUNNING),
        'c35_window_interval_whole_control': _interval_window(n),
        'c35_window_interval64_whole_control': _interval_window(n, extended=True),
    })
    for kind in ('numeric', 'decimal'):
        value = 'id' if kind == 'numeric' else _cast('id', 'Decimal(18, 0)', required=True)
        numeric = g.select(['id', g.alias(_mod('id', _GROUPS), 'p'), g.alias(value, 'v')], 't')
        mean = g.over(g.call('AVG', 'v'), 'p', 'id', _RUNNING)
        if kind == 'decimal':
            mean = _cast(mean, 'Int64')
        result[f'c21_{kind}_control'] = g.select(['id', 'p', g.alias(mean, 'a')], g.derived(numeric, 'q'), order='id')

    duplicate_order = 'p DESC, v ASC, v DESC, id ASC, id DESC'
    unique_order = 'v ASC, id ASC'
    for suffix, partition, order in [('duplicates', 'p, p', duplicate_order),
                                      ('unique_control', 'p', unique_order)]:
        fields = ['id', 'p', g.alias(g.over(g.call('SUM', 'id'), partition, order, _RUNNING), 'a')]
        result['c22_' + suffix] = g.select(fields, g.derived(source, 'q'), order='id')
    result['c22_peer_duplicates'] = _ranking(source, 'v ASC, v DESC')
    result['c22_peer_unique_control'] = _ranking(source, 'v ASC')

    duplicate_sort = 'v ASC, v DESC, p DESC, p ASC, id ASC, id DESC'
    unique_sort = 'v ASC, p DESC, id ASC'
    for limited in (False, True):
        for control in (False, True):
            label = 'c36_' + ('limited' if limited else 'full') + ('_unique_control' if control else '_duplicates')
            result[label] = g.select(['id', 'p', 'v'], g.derived(source, 'q'),
                                     order=unique_sort if control else duplicate_sort,
                                     limit=max(3, n // 2) if limited else None)

    for purpose in ('argument', 'partition', 'order'):
        for control in (False, True):
            label = 'c45_' + purpose + ('_materialized_control' if control else '_nested')
            result[label] = _nested_window(n, purpose, control)
    return result


def schema_and_seeds(n):
    """Reuse review setup: table t contains generated ids 1..n."""
    _size(n)
    return g.schema_and_seeds(n, 'window_checks')


def requirements(label, mode='new'):
    """Return settings, not rendered statements or persisted config files."""
    if mode not in ('new', 'legacy'):
        raise ValueError('Unknown optimizer mode')
    pragmas = [('YqlSelect', 'force' if mode == 'new' else 'disable'),
               ('ydb.WindowFunctionsV2', 'true')]
    # C19/C22 controls isolate their own defects from legacy nullable rank mode.
    flags = ['DisableAnsiRankForNullableKeys' if label.startswith('c20_legacy_')
             else 'AnsiRankForNullableKeys']
    table_config = {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False}
    if label.startswith('c35_window_'):
        # Whole-partition expansion introduces a shared input plus aggregate/join.
        table_config.update(EnableQueryServiceSpilling=True,
                            EnableSpillingInHashJoinShuffleConnections=True,
                            EnableSpillingNodes='All')
    lowering = ('ordinary aggregate after whole-partition window expansion' if label.startswith('c35_window_')
                else 'physical window builder with a non-whole ROWS frame' if label.startswith('c21_')
                else 'ordered native ranking window' if label.startswith('c20_')
                else 'case-specific window or sort lowering')
    return {
        'bug': label.split('_', 1)[0].upper(),
        'base_config': str(_REVIEW / (mode + '.conf')),
        'table_service_config': table_config,
        'pragmas': pragmas,
        'flag_pragmas': flags,
        'node_count': 1,
        'minimum_rows': 8,
        'recommended_rows': 24,
        'result_order_required': label.startswith('c36_'),
        'expected_lowering': lowering,
        'acceptance_scope': 'C35 only; do not gate C21 on this result' if label.startswith('c35_window_') else 'independent bug branch',
        'rank_isolation': 'C20 always retains ORDER BY, including its all-null controls; C19 uses ANSI mode and no nullable-key behavior.',
        'setup_note': 'Use unmodified seed statements with matching server mode; keep all generated text in anonymous files.',
    }


def paired_controls():
    return {
        'c19_unordered': ['c19_ordered_control'],
        'c20_ansi_asc': ['c20_legacy_asc', 'c20_ansi_nonnullable_control'],
        'c20_ansi_desc': ['c20_legacy_desc', 'c20_ansi_nonnullable_control'],
        'c20_ansi_all_null': ['c20_legacy_all_null'],
        'c20_legacy_asc': ['c20_ansi_asc', 'c20_legacy_nonnullable_control'],
        'c20_legacy_desc': ['c20_ansi_desc', 'c20_legacy_nonnullable_control'],
        'c20_legacy_all_null': ['c20_ansi_all_null'],
        'c21_interval_running': ['c21_numeric_control', 'c21_decimal_control'],
        'c21_interval_optional_running': ['c21_interval_running', 'c21_interval_all_null_running'],
        'c21_interval_empty_frame': ['c21_interval_optional_running'],
        'c21_interval64_precise_running': ['c21_numeric_control', 'c21_decimal_control'],
        'c21_interval64_optional_running': ['c21_interval64_precise_running'],
        'c22_duplicates': ['c22_unique_control'],
        'c22_peer_duplicates': ['c22_peer_unique_control'],
        'c36_full_duplicates': ['c36_full_unique_control'],
        'c36_limited_duplicates': ['c36_limited_unique_control'],
        **{'c45_' + purpose + '_nested': ['c45_' + purpose + '_materialized_control']
           for purpose in ('argument', 'partition', 'order')},
    }


def _rows(n, nullable=False, all_null=False):
    return [{'id': i, 'p': i % _GROUPS,
             'v': None if all_null or nullable and i % 5 == 0 else i % 4}
            for i in range(1, n + 1)]


def _rank_expected(n, *, ordered=True, legacy=False, nullable=False, all_null=False, descending=False):
    rows = _rows(n, nullable, all_null)
    expected = {}
    for p in range(_GROUPS):
        group = [row for row in rows if row['p'] == p]
        if ordered:
            group.sort(key=lambda row: (row['v'] is not None, row['v'] or 0), reverse=descending)
        previous = object()
        rank = dense = position = 0
        initialized = False
        for row in group:
            value = row['v']
            if legacy and value is None:
                expected[row['id']] = (rank if initialized else None, dense if initialized else None)
                continue
            position += 1
            if not initialized or ordered and value != previous:
                rank = position
                dense += 1
            initialized = True
            previous = value
            expected[row['id']] = (rank, dense)
    return expected


def _interval_mean(values):
    # Decimal(35,0) division rounds to nearest with ties to even; avoid Double
    # in the model so Interval64 expectations retain all integer precision.
    values = [value for value in values if value is not None]
    if not values:
        return None
    total = sum(values)
    quotient, remainder = divmod(abs(total), len(values))
    if remainder * 2 > len(values) or remainder * 2 == len(values) and quotient % 2:
        quotient += 1
    return -quotient if total < 0 else quotient


def _window_expected(label, n):
    if label.startswith(('c21_', 'c35_window_')):
        extended = 'interval64' in label
        optional = 'optional' in label or 'empty_frame' in label
        all_null = 'all_null' in label
        values = {}
        for i in range(1, n + 1):
            value = 2 ** 53 + i * 13 - (n + 3) if extended else (i % 7 - 3) * 1000003 + i
            if label in ('c21_numeric_control', 'c21_decimal_control'):
                value = i
            values[i] = None if all_null or optional and i % 4 == 0 else value
        expected = {}
        for p in range(_GROUPS):
            group = [i for i in range(1, n + 1) if i % _GROUPS == p]
            for position, i in enumerate(group):
                selected = group
                if 'empty_frame' in label:
                    selected = group[max(0, position - 2):position]
                elif label.startswith('c21_'):
                    selected = group[:position + 1]
                present = [values[j] for j in selected if values[j] is not None]
                # Both interval state and this Decimal control have scale zero.
                expected[i] = ((sum(present) / len(present) if present else None) if 'numeric' in label else
                               _interval_mean(present))
        return expected
    if label.startswith('c22_'):
        expected = {}
        for p in range(_GROUPS):
            group = sorted((i for i in range(1, n + 1) if i % _GROUPS == p), key=lambda i: (i % 4, i))
            total = 0
            for i in group:
                total += i
                expected[i] = total
        return expected
    if label.startswith('c45_argument_'):
        total = sum(3 * i + 1 for i in range(1, n + 1))
        return {i: total for i in range(1, n + 1)}
    expected = {}
    for p in range(_GROUPS):
        group = [i for i in range(1, n + 1) if i % _GROUPS == p]
        if label.startswith('c45_order_'):
            group.reverse()
        total = 0
        for i in group:
            total += i
            expected[i] = total
    return expected


def semantic_oracle(label, rows, n):
    """Return sanitized booleans only; never expose expected/actual row data."""
    _size(n)
    verdict = {'row_count_ok': False, 'identities_ok': False, 'values_ok': False}
    try:
        limited = label.startswith('c36_limited_')
        expected_count = max(3, n // 2) if limited else n
        verdict['row_count_ok'] = len(rows) == expected_count
        ids = [row['id'] for row in rows]
        verdict['identities_ok'] = len(set(ids)) == len(ids) and all(isinstance(i, int) and 1 <= i <= n for i in ids)
        if not limited:
            verdict['identities_ok'] &= set(ids) == set(range(1, n + 1))
        if label.startswith('c36_'):
            wanted = sorted(_rows(n), key=lambda row: (row['v'], -row['p'], row['id']))[:expected_count]
            verdict['values_ok'] = rows == wanted
        elif label.startswith(('c19_', 'c20_')) or label.startswith('c22_peer_'):
            nullable = label.startswith('c20_') and 'nonnullable' not in label
            expected = _rank_expected(n, ordered=label != 'c19_unordered',
                                      legacy=label.startswith('c20_legacy_'), nullable=nullable,
                                      all_null='all_null' in label, descending=label.endswith('_desc'))
            inputs = {row['id']: row for row in _rows(n, nullable, 'all_null' in label)}
            verdict['values_ok'] = all((row['r'], row['d']) == expected[row['id']]
                                       and row['p'] == inputs[row['id']]['p']
                                       and row['v'] == inputs[row['id']]['v'] for row in rows)
            if label.startswith('c19_'):
                row_numbers_ok = True
                for p in range(_GROUPS):
                    group = [row for row in rows if row['p'] == p]
                    row_numbers_ok &= sorted(row['rn'] for row in group) == list(range(1, len(group) + 1))
                    if label == 'c19_ordered_control':
                        for row in group:
                            before = sum(peer['v'] < row['v'] for peer in group)
                            peers = sum(peer['v'] == row['v'] for peer in group)
                            row_numbers_ok &= before < row['rn'] <= before + peers
                verdict['row_numbers_ok'] = row_numbers_ok
        else:
            expected = _window_expected(label, n)
            if label == 'c21_numeric_control':
                verdict['values_ok'] = all(math.isclose(row['a'], expected[row['id']], rel_tol=1e-14, abs_tol=1e-14) for row in rows)
            else:
                verdict['values_ok'] = all(row['a'] == expected[row['id']] for row in rows)
    except (KeyError, TypeError, ValueError, OverflowError):
        verdict['values_ok'] = False
    verdict['semantic_oracle_pass'] = all(verdict.values())
    return verdict
