"""Acceptance generators for the Q15 same-stage wrapper amendment.

Only builder code, aggregate evidence and model verdicts may be persisted.
Rendered inputs, results and physical plans stay in anonymous caller buffers.
The direct-only Q15 branch uses quality_window_checks instead of this profile.
"""
import json
import re

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q15_same_stage_wrappers_v1'


def _int(value):
    return g.typed('Int64', value)


def _chain(kind):
    first_keys = 'p, o' if kind in ('ordered_keys_control', 'subset_keys_control') else 'p'
    first = g.select(['id', 'p', 'o',
                      g.alias(g.over(g.call('ROW_NUMBER'), first_keys, 'id ASC'), 'a')],
                     g.derived(windows._source(), 's'))
    first = g.select(['id', 'p', 'o', 'a'], g.derived(first, 'f'),
                     where=g.op('a', '>', g.typed('Uint64', 1)))
    if kind == 'computed_key_control':
        second_keys = 'q'
        first = g.select(['id', 'a', 'o', g.alias(g.op('p', '+', _int(1)), 'q')],
                         g.derived(first, 'm'))
    elif kind in ('alias_filter', 'payload_map'):
        second_keys = 'q' if kind == 'alias_filter' else 'p'
        key = g.alias('p', 'q') if kind == 'alias_filter' else 'p'
        first = g.select(['id', 'a', 'o', key,
                          g.alias(g.op('a', '+', g.typed('Uint64', 3)), 'payload')],
                         g.derived(first, 'm'))
    else:
        second_keys = 'o, p' if kind == 'ordered_keys_control' else 'p'
    fields = ['id', 'a', g.alias(g.over(g.call('ROW_NUMBER'), second_keys, 'id DESC'), 'b')]
    if kind in ('alias_filter', 'payload_map'):
        fields.append('payload')
    return g.select(fields, g.derived(first, 'w'), order='id')


def recipes(n):
    if type(n) is not int or n < 8:
        raise ValueError('Partition checks require at least eight rows')
    result = {case: query for case, query in windows.recipes(n).items() if case.startswith('q15_')}
    for kind in ('alias_filter', 'payload_map', 'computed_key_control',
                 'ordered_keys_control', 'subset_keys_control'):
        result['q15_' + kind] = _chain(kind)
    return result


def schema_and_seeds(n, case):
    return g.schema_and_seeds(n, case)


def metadata(case, n):
    result = windows.metadata(case, n)
    result.update(generator_version=GENERATOR_VERSION,
                  acceptance_scope='Q15 transparent-wrapper amendment, not the direct-only branch',
                  recommended_row_count=61,
                  additional_ir_admission_required=case in ('q15_alias_filter', 'q15_payload_map',
                                                           'q15_computed_key_control'))
    return result


def expected_rows(case, n):
    if case in windows.recipes(n):
        return windows.expected_rows(case, n)
    source = [{'id': i, 'p': i % 3, 'o': i % 5} for i in range(1, n + 1)]
    combined = case in ('q15_ordered_keys_control', 'q15_subset_keys_control')
    first_key = lambda row: (row['p'], row['o']) if combined else (row['p'],)
    for row in source:
        row['a'] = sum(other['id'] <= row['id'] and first_key(other) == first_key(row)
                       for other in source)
    kept = [row for row in source if row['a'] > 1]
    second_key = (lambda row: (row['o'], row['p'])) if case == 'q15_ordered_keys_control' else (
        (lambda row: (row['p'] + 1,)) if case == 'q15_computed_key_control' else (lambda row: (row['p'],)))
    result = []
    for row in kept:
        item = {'id': row['id'], 'a': row['a'],
                'b': sum(other['id'] >= row['id'] and second_key(other) == second_key(row) for other in kept)}
        if case in ('q15_alias_filter', 'q15_payload_map'):
            item['payload'] = row['a'] + 3
        result.append(item)
    return result


def check_rows(case, rows, n):
    canonical = lambda data: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in data)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_partition_rank_oracle': True, 'quality_improvement_verified': False}


def _summary(text):
    result = windows.physical_ast_summary(text)
    _, nodes = windows.parse_physical_graph(text)
    result['retained_filter_count'] = sum(bool(node) and node[0] in (
        'Filter', 'OrderedFilter', 'WideFilter', 'WideFilterWithLimit', 'WideFilterBlocks')
        for node in nodes if isinstance(node, list) and node and isinstance(node[0], str))
    return result


def check_physical_ast(case, text, baseline=None):
    summary = _summary(text)
    positive = case in ('q15_same_partition', 'q15_intervening_filter', 'q15_override_enabled',
                        'q15_alias_filter', 'q15_payload_map')
    expected = 0 if case == 'q15_keyless_control' else 1 if positive else 2
    assertions = {'expected_shuffle_count': summary['callables']['DqCnHashShuffle'] == expected}
    if case in ('q15_intervening_filter', 'q15_alias_filter', 'q15_payload_map',
                'q15_computed_key_control', 'q15_ordered_keys_control', 'q15_subset_keys_control'):
        assertions['dependent_filter_retained'] = summary['retained_filter_count'] > 0
    if baseline is not None:
        before = _summary(baseline) if isinstance(baseline, str) else baseline
        if (not isinstance(before, dict) or before.get('physical_ast_parsed') is not True
            or not re.fullmatch('[0-9a-f]{64}', before.get('physical_ast_sha256', ''))
            or not isinstance(before.get('callables'), dict)
            or type(before['callables'].get('DqCnHashShuffle')) is not int
            or before['callables']['DqCnHashShuffle'] < 0):
            raise ValueError('Baseline partition summary is not validated evidence')
        summary['baseline_physical_ast_sha256'] = before['physical_ast_sha256']
        summary['baseline_shuffle_count'] = before['callables']['DqCnHashShuffle']
        if positive:
            assertions['baseline_redundant_shuffle_present'] = before['callables']['DqCnHashShuffle'] > expected
    summary['structural_assertions'] = assertions
    summary['structural_oracle_pass'] = bool(assertions) and all(assertions.values())
    summary['quality_improvement_verified'] = False
    summary['additional_ir_admission_required'] = case in (
        'q15_alias_filter', 'q15_payload_map', 'q15_computed_key_control')
    return summary
