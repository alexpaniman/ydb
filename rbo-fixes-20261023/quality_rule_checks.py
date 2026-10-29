"""Compositional Q05/Q12/Q21 acceptance checks; all rendered data stays in RAM.

The native harness must separately establish success, correct results, the
intended target plan shape and improvement over baseline. This module has no
execution or persistence entry point, and exports only sanitized plan evidence.
"""
import json

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'quality_topn_json_exists_independent_models_v1'


def _int(value):
    return g.typed('Int64', value)


def _mod(value, divisor):
    return g.call('Unwrap', g.op(value, '%', _int(divisor)))


def _exists(n, *, negative=False, limit=1, offset=None, generic=False):
    predicate = g.op('b.id', '=', g.op('a.id', '*', _int(2)))
    subquery = g.select(['b.id'], 't AS b', where=predicate, limit=limit)
    if offset is not None:
        subquery += ' OFFSET ' + str(offset)
    condition = ('NOT ' if negative else '') + 'EXISTS (' + subquery + ')'
    if generic:
        return g.select(['a.id AS id', g.alias(condition, 'present')], 't AS a', order='a.id')
    return g.select(['a.id AS id'], 't AS a', where=condition, order='a.id')


def recipes(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('Quality checks require at least eight rows')
    result = {}
    limit = max(3, n // 2)
    for direction in ('asc', 'desc'):
        for placement in ('default', 'first', 'last'):
            order = 'id ' + direction.upper() + ('' if placement == 'default' else ' NULLS ' + placement.upper())
            result['q05_required_' + direction + '_' + placement] = g.select(['id'], 't', order=order, limit=limit)
        for placement in ('first', 'last'):
            result['q05_nullable_' + direction + '_' + placement] = g.select(
                ['id'], 'nq', order='id ' + direction.upper() + ' NULLS ' + placement.upper(), limit=limit)
    result['q05_nonkey_control'] = g.select(['id'], 't', order='k ASC, id DESC', limit=limit)

    path = g.quote('$.x')
    value = g.call('JSON_VALUE', 'doc', path)
    exists = g.call('JSON_EXISTS', 'doc', path)
    result['q12_json_value'] = g.select(['id'], 'j', where=g.op(value, '=', g.typed('Utf8', '3')), order='id')
    result['q12_json_exists'] = g.select(['id'], 'j', where=exists, order='id')
    result['q12_not_json_exists'] = g.select(['id'], 'j', where='NOT ' + exists, order='id')
    strict_value = g.call('JSON_VALUE', 'doc', path + ' RETURNING Utf8')
    result['q12_explicit_returning_control'] = g.select(['id'], 'j',
        where=g.op(strict_value, '=', g.typed('Utf8', '3')), order='id')
    residual = g.op(_mod('id', 2), '=', _int(0))
    result['q12_native_with_residual'] = g.select(['id'], 'j',
        where=g.op(g.op(value, '=', g.typed('Utf8', '3')), 'AND', residual), order='id')
    result['q12_optional_boolean'] = g.select(['id'], 'j',
        where=g.op(g.op('v', '>', _int(2)), 'OR', exists), order='id')
    result['q12_pushdown_disabled_control'] = result['q12_json_value']

    for negative in (False, True):
        for generic in (False, True):
            for size in (1, 3):
                case = 'q21_' + ('not_exists' if negative else 'exists')
                case += '_generic' if generic else '_filter'
                result[case + '_' + str(size)] = _exists(n, negative=negative, generic=generic, limit=size)
    result['q21_zero_control'] = _exists(n, limit=0, generic=True)
    result['q21_offset_control'] = _exists(n, offset=1, generic=True)
    result['q21_unlimited_control'] = _exists(n, limit=None, generic=True)
    subquery = g.select(['b.id'], 't AS b', where=g.op(_mod('b.id', 2), '=', _int(0)), order='b.id', limit=3)
    result['q21_in_control'] = g.select(['a.id AS id'], 't AS a',
        where=g.op('a.id', 'IN', '(' + subquery + ')'), order='a.id')
    one = g.select(['b.id'], 't AS b', where=g.op('b.id', '=', 'a.id'), limit=1)
    result['q21_scalar_control'] = g.select(['a.id AS id', g.alias('(' + one + ')', 'value')], 't AS a', order='a.id')
    return result


def _document(i):
    # Mix numbers, strings, JSON null, missing members, containers and SQL NULL.
    kind = i % 6
    if kind == 0:
        return None
    return ({'x': 3}, {'x': '3'}, {'x': None}, {}, {'x': [3]})[kind - 1]


def schema_and_seeds(n, case):
    if case.startswith('q12_'):
        definitions = ['id Int64 NOT NULL', 'doc JsonDocument', 'v Int64', 'PRIMARY KEY (id)']
        schema = 'CREATE TABLE j (' + ', '.join(definitions) + ') WITH (STORE = COLUMN, PARTITION_COUNT = 4);'
        arms = []
        for i in range(1, n + 1):
            document = _document(i)
            value = (g.call('Nothing', 'JsonDocument?') if document is None else
                     g.call('Just', g.typed('JsonDocument', json.dumps(document, separators=(',', ':')))))
            optional = g.call('Nothing', 'Int64?') if i % 4 == 0 else g.call('Just', _int(i % 5))
            arms.append(g.select([g.alias(_int(i), 'id'), g.alias(value, 'doc'), g.alias(optional, 'v')]))
        return schema, ['UPSERT INTO j ' + g.combine(arms)]
    schema, seeds = g.schema_and_seeds(n, case)
    if case.startswith('q05_nullable_'):
        schema += ' CREATE TABLE nq (id Int64, PRIMARY KEY (id));'
        arms = [g.select([g.alias(g.call('Nothing', 'Int64?') if i == 1 else g.call('Just', _int(i)), 'id')])
                for i in range(1, n + 1)]
        seeds.append('UPSERT INTO nq ' + g.combine(arms))
    return schema, seeds


def metadata(case, n):
    config = {'EnableFallbackToYqlOptimizer': False}
    pragmas = []
    if case.startswith('q12_'):
        config.update(EnableOlapSink=True, AllowOlapDataQuery=True)
        pragmas += [('kikimr.OptEnableOlapPushdown', 'false' if case == 'q12_pushdown_disabled_control' else 'true')]
    if case.startswith('q21_'):
        config.update(EnableQueryServiceSpilling=True, EnableSpillingInHashJoinShuffleConnections=True,
                      EnableSpillingNodes='All', DqHashOperatorsUseBlocks=False)
    return {'generator_version': GENERATOR_VERSION, 'table_service_overrides': config,
            'pragmas': pragmas, 'node_count': 1, 'peak_native_concurrency': 1,
            'quality_requires_separate_plan_evidence': True}


def expected_rows(case, n):
    if case.startswith('q05_'):
        ids = list(range(1, n + 1))
        descending = '_desc_' in case or case == 'q05_nonkey_control'
        if case.startswith('q05_nullable_'):
            ids = list(range(2, n + 1))
        ids.sort(reverse=descending)
        if case.startswith('q05_nullable_'):
            ids.insert(0 if case.endswith('_first') else len(ids), None)
        return [{'id': i} for i in ids[:max(3, n // 2)]]
    if case.startswith('q12_'):
        result = []
        for i in range(1, n + 1):
            document = _document(i)
            exists = None if document is None else 'x' in document
            value_matches = document is not None and document.get('x') in (3, '3')
            if case == 'q12_json_exists':
                keep = exists is True
            elif case == 'q12_not_json_exists':
                keep = exists is False
            elif case == 'q12_explicit_returning_control':
                keep = document is not None and document.get('x') == '3'
            elif case == 'q12_optional_boolean':
                left = None if i % 4 == 0 else i % 5 > 2
                keep = left is True or exists is True
            else:
                keep = value_matches and (case != 'q12_native_with_residual' or i % 2 == 0)
            if keep:
                result.append({'id': i})
        return result
    if case == 'q21_in_control':
        return [{'id': i} for i in (2, 4, 6) if i <= n]
    if case == 'q21_scalar_control':
        return [{'id': i, 'value': i} for i in range(1, n + 1)]
    if case.startswith('q21_'):
        result = []
        for i in range(1, n + 1):
            present = i * 2 <= n and case not in ('q21_zero_control', 'q21_offset_control')
            if '_not_exists_' in case:
                present = not present
            if '_generic_' in case or case in ('q21_zero_control', 'q21_offset_control', 'q21_unlimited_control'):
                result.append({'id': i, 'present': present})
            elif present:
                result.append({'id': i})
        return result
    raise ValueError('Unknown quality rule case')


def check_rows(case, rows, n):
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values)
    expected = expected_rows(case, n)
    # Ordered top-N must return both the correct set and the requested order.
    passed = rows == expected if case.startswith('q05_') else canonical(rows) == canonical(expected)
    return {'semantic_oracle_pass': passed, 'independent_multiset_oracle': True,
            'result_order_checked': case.startswith('q05_'), 'quality_improvement_verified': False}


def check_physical_ast(case, text, baseline=None):
    # Reuse the strict reader and validated sanitized-baseline acceptance.
    summary = windows.check_physical_ast(case, text, baseline)
    counters = summary['callables']
    assertions = {}
    if case.startswith('q05_'):
        eligible = case.startswith('q05_required_') or case in ('q05_nullable_asc_first', 'q05_nullable_desc_last')
        pushed = any('ItemsLimit' in names and ('Sorted' in names or 'Reverse' in names)
                     for names in summary['row_read_setting_names'])
        assertions['storage_topn_eligibility_preserved'] = pushed == eligible
    elif case.startswith('q12_'):
        if case in ('q12_explicit_returning_control', 'q12_pushdown_disabled_control'):
            assertions['ineligible_json_value_not_native'] = counters['KqpOlapJsonValue'] == 0
        elif case in ('q12_json_value', 'q12_native_with_residual'):
            assertions['native_json_value_present'] = counters['KqpOlapJsonValue'] > 0
        else:
            assertions['native_json_exists_present'] = counters['KqpOlapJsonExists'] > 0
    elif case.startswith('q21_') and not case.endswith('_control'):
        # The EXISTS input has no user window. A residual physical ranking
        # chain exposes per-domain LIMIT lowering that should have disappeared.
        chains = sum(counters[name] for name in ('ChainMap', 'Chain1Map', 'WideChainMap', 'WideChain1Map'))
        assertions['existence_limit_has_no_ranking_chain'] = chains == 0
        assertions['existence_limit_has_no_frame_queue'] = counters['QueueCreate'] == 0
    if baseline is not None and case.startswith('q12_') and case not in ('q12_explicit_returning_control', 'q12_pushdown_disabled_control'):
        before = summary['baseline_callables']
        metric = 'KqpOlapJsonValue' if case in ('q12_json_value', 'q12_native_with_residual') else 'KqpOlapJsonExists'
        assertions['native_json_lowering_increased'] = before[metric] < counters[metric]
    if baseline is not None and case.startswith('q21_') and not case.endswith('_control'):
        before = summary['baseline_callables']
        assertions['baseline_ranking_work_removed'] = sum(before[name] for name in (
            'ChainMap', 'Chain1Map', 'WideChainMap', 'WideChain1Map')) > 0
    summary['structural_assertions'] = assertions
    summary['structural_oracle_pass'] = bool(assertions) and all(assertions.values())
    summary['quality_improvement_verified'] = False
    return summary
