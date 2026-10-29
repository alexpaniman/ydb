"""Q11 compositional builders and independent row/branch-access contracts.

Render only into anonymous buffers. Physical branch ranges/index reads are
observations, not a substitute for matched baseline semantics. Volatile-call
placement and exact UnionAll Ordered-state identity still need a direct IR
adapter; final ORDER BY checks the externally visible ordering contract only.
"""
import hashlib
import json
import re

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'quality_union_independent_models_v1'
CASES = tuple('q11_' + variant for variant in (
    'primary_range', 'secondary_index', 'nullable_index', 'bag_duplicates',
    'ordered_result', 'empty_result', 'shared_producer_control', 'widened_type_control',
    'arithmetic_control', 'fallible_control', 'unknown_call_control'))


def _int(value):
    return g.typed('Int64', value)


def _variant(case):
    if case not in CASES:
        raise ValueError('Unknown union quality case')
    return case.removeprefix('q11_')


def _rows(n, table, case):
    offset = 0 if table == 'u' or case == 'q11_bag_duplicates' else n // 2
    return [{'id': i + offset, 'k': None if (i + offset) % 4 == 0 else (i + offset) % 3,
             'payload': 'p' + str(i + offset)} for i in range(1, n + 1)]


def schema_and_seeds(n, case):
    _variant(case)
    definitions = ['id Int64 NOT NULL', 'k Int64', 'payload Utf8', 'PRIMARY KEY (id)',
                   'INDEX byk GLOBAL ON (k) COVER (payload)']
    schema = ''.join('CREATE TABLE ' + table + ' (' + ', '.join(definitions) + ');' for table in ('u', 'v'))
    seeds = []
    for table in ('u', 'v'):
        arms = []
        for row in _rows(n, table, case):
            optional = g.call('Nothing', 'Int64?') if row['k'] is None else g.call('Just', _int(row['k']))
            arms.append(g.select([g.alias(_int(row['id']), 'id'), g.alias(optional, 'k'),
                                  g.alias(g.call('Just', g.typed('Utf8', row['payload'])), 'payload')]))
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(arms))
    return schema, seeds


def _predicate(n, case):
    if case == 'q11_secondary_index':
        return g.op('x', '=', _int(1))
    if case == 'q11_nullable_index':
        return 'x IS NULL'
    if case == 'q11_unknown_call_control':
        return g.call('String::Contains', g.call('CAST', 'payload AS String'), g.quote('p'))
    value = 'x'
    if case == 'q11_arithmetic_control':
        value = g.op(value, '+', _int(1))
    elif case == 'q11_fallible_control':
        value = g.call('Ensure', value, g.op(value, '>', _int(0)), g.quote('guard'))
    threshold = n * 3 if case == 'q11_empty_result' else max(2, n // 2)
    return g.op(value, '>=', _int(threshold))


def recipes(n):
    if type(n) is not int or n < 8:
        raise ValueError('Union quality checks require at least eight rows')
    result = {}
    for case in CASES:
        arms = []
        for index, table in enumerate(('u', 'u' if case == 'q11_shared_producer_control' else 'v')):
            nullable = case in ('q11_secondary_index', 'q11_nullable_index') or case == 'q11_widened_type_control' and index == 1
            arms.append(g.select([g.alias('k' if nullable else 'id', 'x'), 'id', 'payload'], table))
        result[case] = g.select(['x', 'id', 'payload'], g.derived(g.combine(arms), 'combined'),
                                where=_predicate(n, case),
                                order='id, payload' if case == 'q11_ordered_result' else None)
    return result


def metadata(case, n):
    _variant(case)
    return {'generator_version': GENERATOR_VERSION,
            'table_service_overrides': {'EnableFallbackToYqlOptimizer': False},
            'pragmas': [('ydb.OptDisableAutoIndexSelection', 'false'), ('kikimr.CostBasedOptimizationLevel', '0')],
            'quality_requires_separate_plan_evidence': True, 'node_count': 1,
            'peak_native_concurrency': 1, 'native_target_reachability_pending': True}


def expected_rows(case, n):
    _variant(case)
    result = []
    for index, table in enumerate(('u', 'u' if case == 'q11_shared_producer_control' else 'v')):
        nullable = case in ('q11_secondary_index', 'q11_nullable_index') or case == 'q11_widened_type_control' and index == 1
        for row in _rows(n, table, case):
            value = row['k'] if nullable else row['id']
            if case == 'q11_secondary_index':
                keep = value == 1
            elif case == 'q11_nullable_index':
                keep = value is None
            elif case == 'q11_unknown_call_control':
                keep = 'p' in row['payload']
            else:
                threshold = n * 3 if case == 'q11_empty_result' else max(2, n // 2)
                keep = value is not None and value + (case == 'q11_arithmetic_control') >= threshold
            if keep:
                result.append({'x': value, 'id': row['id'], 'payload': row['payload']})
    if case == 'q11_ordered_result':
        result.sort(key=lambda row: (row['id'], row['payload']))
    return result


def check_rows(case, rows, n):
    expected = expected_rows(case, n)
    ordered = case == 'q11_ordered_result'
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values)
    return {'semantic_oracle_pass': rows == expected if ordered else canonical(rows) == canonical(expected),
            'independent_multiset_oracle': True, 'result_order_checked': ordered,
            'quality_improvement_verified': False}


def _atom(value, resolve):
    value = resolve(value)
    if not isinstance(value, str):
        raise ValueError('Expected an AST atom')
    if value.startswith("'"):
        value = value[1:]
    return json.loads(value) if value.startswith('"') else value


def physical_ast_summary(text):
    resolve, nodes = windows.parse_physical_graph(text)
    reads = []
    ensure_count = 0
    for node in nodes:
        if not node:
            continue
        if node[0] == 'Ensure':
            ensure_count += 1
        if node[0] != 'KqpRowsSourceSettings':
            continue
        if len(node) not in (5, 6):
            raise ValueError('Unknown row source shape')
        table = resolve(node[1])
        if not isinstance(table, list) or len(table) != 5 or table[0] != 'KqpTable':
            raise ValueError('Unknown source table')
        path = _atom(table[1], resolve)
        match = re.search(r'/(u|v)(/byk/indexImplTable)?$', path)
        if not match:
            raise ValueError('Unexpected source table in union case')
        ranges = resolve(node[4])
        reads.append({'branch_table': match.group(1), 'secondary_index': bool(match.group(2)),
                      'range_expression_present': isinstance(ranges, list) and bool(ranges) and ranges[0] != 'Void'})
    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(), 'physical_ast_parsed': True,
            'union_summary_version': 1, 'row_sources': sorted(reads, key=lambda item: json.dumps(item, sort_keys=True)),
            'ensure_dag_nodes': ensure_count}


def _baseline(value):
    if isinstance(value, str):
        return physical_ast_summary(value)
    if (not isinstance(value, dict) or value.get('physical_ast_parsed') is not True
        or value.get('union_summary_version') != 1 or not isinstance(value.get('physical_ast_sha256'), str)
        or not re.fullmatch('[0-9a-f]{64}', value['physical_ast_sha256'])
        or type(value.get('ensure_dag_nodes')) is not int or value['ensure_dag_nodes'] < 0
        or not isinstance(value.get('row_sources'), list)):
        raise ValueError('Invalid baseline union summary')
    for item in value['row_sources']:
        if (not isinstance(item, dict) or set(item) != {'branch_table', 'secondary_index', 'range_expression_present'}
            or item['branch_table'] not in ('u', 'v') or type(item['secondary_index']) is not bool
            or type(item['range_expression_present']) is not bool):
            raise ValueError('Invalid baseline branch source')
    return {key: value[key] for key in ('physical_ast_sha256', 'physical_ast_parsed', 'union_summary_version',
                                       'row_sources', 'ensure_dag_nodes')}


def check_physical_ast(case, text, baseline=None):
    variant = _variant(case)
    summary = physical_ast_summary(text)
    reads = summary['row_sources']
    target = not variant.endswith('_control')
    assertions = {}
    if target:
        assertions['both_branches_reachable'] = len(reads) == 2 and {item['branch_table'] for item in reads} == {'u', 'v'}
        assertions['predicate_reaches_both_access_paths'] = len(reads) == 2 and all(item['range_expression_present'] for item in reads)
        if case in ('q11_secondary_index', 'q11_nullable_index'):
            assertions['both_branches_use_secondary_index'] = len(reads) == 2 and all(item['secondary_index'] for item in reads)
    elif case == 'q11_shared_producer_control':
        assertions['shared_source_retained'] = bool(reads) and all(item['branch_table'] == 'u' for item in reads)
    else:
        assertions['ineligible_filter_not_installed_as_branch_range'] = len(reads) == 2 and not any(item['range_expression_present'] for item in reads)
        if case == 'q11_fallible_control':
            assertions['fallible_guard_present'] = summary['ensure_dag_nodes'] > 0
    if baseline is not None:
        before = _baseline(baseline)
        summary['baseline_physical_ast_sha256'] = before['physical_ast_sha256']
        summary['baseline_bounded_sources'] = sum(item['range_expression_present'] for item in before['row_sources'])
        summary['baseline_secondary_sources'] = sum(item['secondary_index'] for item in before['row_sources'])
        if target:
            assertions['branch_range_pushdown_increased'] = summary['baseline_bounded_sources'] < sum(item['range_expression_present'] for item in reads)
        elif case == 'q11_fallible_control':
            assertions['guard_evaluation_sites_preserved'] = summary['ensure_dag_nodes'] == before['ensure_dag_nodes']
    summary['structural_assertions'] = assertions
    summary['structural_oracle_pass'] = bool(assertions) and all(assertions.values())
    summary['evaluation_count_verified'] = False
    summary['quality_improvement_verified'] = False
    return summary
