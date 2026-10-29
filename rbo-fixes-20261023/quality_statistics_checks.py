"""Compositional quality checks; rendered workloads and plans stay in caller RAM.

Planner assertions are separate from row semantics. Unreachable operator/helper
contracts are described by contract_specs(), never treated as native successes.
"""
from collections import Counter
import hashlib
import json
import math

import generate_and_verify as g
from quality_window_checks import parse_physical_graph

CASE_SPECIFIC_SCHEMA = True
VERSION = 'quality_statistics_and_settings_v1'


def _value(value):
    return g.typed('Int64', value)


def _hint(kind, aliases, value):
    return kind + '(' + ' '.join(aliases) + ' # ' + str(value) + ')'


def recipes(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('At least eight rows required')
    result = {}
    for filtered in (False, True):
        source = g.select(['id', 'v'], 'qs', where=g.op('id', '<=', _value(n // 2)) if filtered else None)
        ranked = g.select(['id', g.alias(g.over(g.call('ROW_NUMBER'), None, 'id ASC'), 'rn')],
                          g.derived(source, 's'))
        result['q08_' + ('nonunit_input' if filtered else 'unit_input_control')] = g.select(
            ['id', 'rn'], g.derived(ranked, 'w'), where=g.op('rn', '<=', g.typed('Uint64', 3)), order='id')
    for empty in (False, True):
        result['q09_scalar_' + ('empty' if empty else 'nonempty')] = g.select(
            [g.alias(g.call('COUNT', '*'), 'c'), g.alias(g.call('SUM', 'v'), 's')], 'qs',
            where=g.op('id', '<', _value(0)) if empty else None)
    result['q09_grouped_control'] = g.select(['v', g.alias(g.call('COUNT', '*'), 'c')], 'qs', group='v')
    for suffix in ('composite_rows', 'composite_bytes', 'unrelated_control'):
        result['q20_' + suffix] = g.select([g.alias('l.id', 'l_id'), g.alias('r.id', 'r_id')],
            'qs AS l INNER JOIN qr AS r ON ' + g.op('l.v', '=', 'r.v'), order='l.id, r.id')
    for value in ('unset', 'zero', 'positive'):
        for shape in ('ordinary', 'sorted_limit'):
            result['q26_' + value + '_' + shape] = g.select(['id', 'v'], 'qs', order='id',
                limit=3 if shape == 'sorted_limit' else None)
    return result


def schema_and_seeds(n, case):
    tables = ('qs', 'qr') if case.startswith('q20_') else ('qs',)
    schema, seeds = [], []
    for table in tables:
        definitions = ['id Int64 NOT NULL', 'v Int64 NOT NULL', 'PRIMARY KEY (id)']
        schema.append('CREATE TABLE ' + table + ' (' + ', '.join(definitions) + ');')
        arms = [g.select([g.alias(_value(i), 'id'), g.alias(_value(i % 3), 'v')])
                for i in range(1, n + 1)]
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(arms))
    return ''.join(schema), seeds


def metadata(case, n):
    config = {'EnableFallbackToYqlOptimizer': False}
    pragmas = [('ydb.CostBasedOptimizationLevel', '1')]
    if case.startswith(('q08_', 'q09_')):
        pragmas += [('ydb.OptimizerHints', _hint('Rows', ['qs'], 1000) + ' ' + _hint('Bytes', ['qs'], 16000))]
    if case.startswith('q08_'):
        pragmas += [('ydb.WindowFunctionsV2', 'true')]
    if case.startswith('q20_'):
        config['EnableKqpDataQueryStreamIdxLookupJoin'] = False
        aliases = ['absent'] if case.endswith('unrelated_control') else ['l', 'r']
        hints = [_hint('Rows', ['l'], 1000), _hint('Rows', ['r'], 1000),
                 _hint('Bytes', ['l'], 16000), _hint('Bytes', ['r'], 16000)]
        hints.append(_hint('Bytes' if case.endswith('composite_bytes') else 'Rows', aliases,
                           1552 if case.endswith('composite_bytes') else 97))
        pragmas += [('ydb.OptimizerHints', ' '.join(hints))]
    if case.startswith('q26_'):
        setting = case.split('_')[1]
        if setting != 'unset':
            pragmas += [('ydb.MaxSequentialReadsInFlight', '0' if setting == 'zero' else '4')]
    return {'generator_version': VERSION, 'table_service_overrides': config,
            'pragmas': pragmas, 'node_count': 1, 'peak_native_concurrency': 1,
            'quality_requires_query_plan_evidence': not case.startswith('q26_'),
            'quality_requires_separate_plan_evidence': case.startswith('q26_')}


def expected_rows(case, n):
    if case.startswith('q08_'):
        return [{'id': i, 'rn': i} for i in range(1, 4)]
    if case.startswith('q09_scalar_'):
        return [{'c': 0, 's': None}] if case.endswith('empty') and not case.endswith('nonempty') else [
            {'c': n, 's': sum(i % 3 for i in range(1, n + 1))}]
    if case == 'q09_grouped_control':
        return [{'v': value, 'c': sum(i % 3 == value for i in range(1, n + 1))} for value in range(3)]
    if case.startswith('q20_'):
        return [{'l_id': i, 'r_id': j} for i in range(1, n + 1) for j in range(1, n + 1) if i % 3 == j % 3]
    if case.startswith('q26_'):
        return [{'id': i, 'v': i % 3} for i in range(1, (3 if case.endswith('sorted_limit') else n) + 1)]
    raise ValueError('Unknown quality case')


def check_rows(case, rows, n):
    canonical = lambda values: Counter(json.dumps(row, sort_keys=True) for row in values)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n))}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate plan key')
        result[key] = value
    return result


def _plan_nodes(text):
    plan = json.loads(text, object_pairs_hook=_unique_object)
    if not isinstance(plan, dict) or not isinstance(plan.get('SimplifiedPlan'), dict):
        raise ValueError('Expected the new RBO simplified query plan')
    todo, nodes = [plan['SimplifiedPlan']], []
    while todo:
        node = todo.pop()
        if not isinstance(node, dict):
            raise ValueError('Invalid plan node')
        nodes.append(node)
        children = node.get('Plans', [])
        if not isinstance(children, list):
            raise ValueError('Invalid plan children')
        todo.extend(children)
    return nodes


def _operator(node):
    operators = node.get('Operators', [])
    return operators[0] if len(operators) == 1 and isinstance(operators[0], dict) else None


def _estimate(operator, field):
    value = float(operator[field])
    if not math.isfinite(value) or value < 0:
        raise ValueError('Nonfinite or negative optimizer estimate')
    return value


def check_query_plan(case, text, n):
    nodes = _plan_nodes(text)
    assertions, estimates = {}, []
    if case.startswith('q08_'):
        candidates = []
        for node in nodes:
            if node.get('Node Type') != 'Filter' or not _operator(node):
                continue
            children = node.get('Plans', [])
            while len(children) == 1 and not _operator(children[0]):
                children = children[0].get('Plans', [])
            if len(children) == 1 and children[0].get('Node Type') == 'Window':
                candidates.append((_operator(node), _operator(children[0])))
        assertions['retained_filter_above_window'] = len(candidates) == 1 and candidates[0][1] is not None
        if assertions['retained_filter_above_window']:
            outer, inner = candidates[0]
            before, after = _estimate(inner, 'E-Rows'), _estimate(outer, 'E-Rows')
            estimates.append({'input_rows': before, 'output_rows': after, 'local_selectivity': 0.4})
            assertions['local_selectivity_applied_once'] = before > 0 and math.isclose(after, before * 0.4, rel_tol=1e-6)
            if case == 'q08_nonunit_input':
                assertions['input_was_previously_filtered'] = 0 < before < 1000
    elif case.startswith('q09_'):
        candidates = [_operator(node) for node in nodes if _operator(node)
                      and 'Aggregation' in _operator(node)]
        if case == 'q09_grouped_control':
            selected = [op for op in candidates if op.get('GroupBy') and op.get('Phase') != 'Intermediate']
            assertions['grouped_control_retained'] = bool(selected)
            assertions['grouped_not_forced_to_one'] = bool(selected) and all(_estimate(op, 'E-Rows') != 1 for op in selected)
        else:
            selected = [op for op in candidates if not op.get('GroupBy') and op.get('Phase') in ('Undefined', 'Final')]
            assertions['scalar_final_present'] = bool(selected)
            assertions['scalar_estimate_exactly_one'] = bool(selected) and all(_estimate(op, 'E-Rows') == 1 for op in selected)
            assertions['finite_positive_scalar_width'] = bool(selected) and all(_estimate(op, 'E-Size') > 0 for op in selected)
        estimates = [{'rows': _estimate(op, 'E-Rows'), 'bytes': _estimate(op, 'E-Size')} for op in selected]
    elif case.startswith('q20_'):
        selected = [_operator(node) for node in nodes if 'Join' in node.get('Node Type', '') and _operator(node)]
        assertions['one_regular_join'] = len(selected) == 1
        if len(selected) == 1:
            rows, size = _estimate(selected[0], 'E-Rows'), _estimate(selected[0], 'E-Size')
            estimates = [{'rows': rows, 'bytes': size}]
            if case.endswith('composite_rows'):
                assertions['composite_rows_hint_bound'] = rows == 97
            elif case.endswith('composite_bytes'):
                assertions['composite_bytes_hint_bound'] = size == 1552
            else:
                assertions['unrelated_hint_not_bound'] = rows != 97
    else:
        raise ValueError('Unknown plan-quality case')
    accepted = bool(assertions) and all(assertions.values())
    return {'query_plan_sha256': hashlib.sha256(text.encode()).hexdigest(), 'query_plan_parsed': True,
            'quality_assertions': assertions, 'optimizer_estimates': estimates,
            'quality_acceptance_pass': accepted,
            # A passing fixed estimate does not establish a baseline delta.
            'quality_improvement_verified': False}


def check_physical_ast(case, text):
    resolve, nodes = parse_physical_graph(text)
    settings = []
    for node in nodes:
        if not node or node[0] != 'KqpRowsSourceSettings':
            continue
        if len(node) <= 3 or not isinstance(resolve(node[3]), list):
            raise ValueError('Invalid row source settings')
        found = []
        for item in resolve(node[3]):
            item = resolve(item)
            if isinstance(item, list) and item and str(resolve(item[0])).strip("'\"") == 'Sequential':
                if len(item) != 2:
                    raise ValueError('Invalid sequential setting')
                found.append(int(str(resolve(item[1])).strip("'\"")))
        if len(found) > 1:
            raise ValueError('Duplicate sequential setting')
        settings.append(found)
    expected = [] if '_unset_' in case else [0 if '_zero_' in case else 4]
    accepted = len(settings) == 1 and settings[0] == expected
    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(), 'physical_ast_parsed': True,
            'row_source_count': len(settings), 'sequential_read_settings': settings,
            'quality_acceptance_pass': accepted,
            # Before/fixed provenance and semantic acceptance are checked by
            # the series evaluator before an improvement can be attributed.
            'quality_improvement_verified': False}


def contract_specs():
    """Helper/IR observations required where SQL cannot prove the contract."""
    return {
        'Q14': {'interface': 'ConvertOptimizedTree then AssignStage',
                'matrix': {'algorithm': ['Grace', 'ReverseBlock', 'Map'], 'left_shuffle': ['absent', 'empty', 'nonempty'],
                           'right_shuffle': ['absent', 'empty', 'nonempty']},
                'required_observations': ['typed ordered keys preserved', 'Map versus Shuffle edges', 'four-node row oracle']},
        'Q16': {'interface': 'TDisableBlocksOnColumnsLimitRule with observed TypeCtx.BlockEngineMode',
                'matrix': {'storage': ['row', 'column', 'mixed', 'unrelated_column_root'], 'width': [0, 1, 3],
                           'threshold': [0, 2], 'blocks_initially_enabled': [False, True]},
                'required_observations': ['actual mode before/after', 'visited shared subtree', 'WindowFunctionsV2 guard unchanged']},
        'Q18': {'interface': 'MergeColumnStatistics and ApplyColumnStatistics',
                'matrix': {'field_kind': ['count_min_sketch', 'width_histogram', 'type', 'ndv_zero', 'hll_zero',
                                        'tuple_columns', 'tuple_types', 'height_histogram', 'tuple_sketch'],
                           'cache': ['absent', 'existing_same_column', 'existing_other_column'], 'repeat': [1, 2]},
                'required_observations': ['stored pointer identity', 'incoming fields installed', 'absent fields preserved'],
                'sql_reachable': False},
        'Q19': {'interface': 'statistics future completion and ApplyColumnStatistics',
                'matrix': {'response_order': ['success_error', 'error_success', 'success_warning', 'all_success', 'all_error'],
                           'future_exception': [False, True]},
                'required_observations': ['all issues retained', 'successful fields applied on uncached table', 'future exception propagated'],
                'sql_reachable': False},
        'Q25': {'interface': 'Map definition state through fusion/copy/rebinding/pushdown',
                'matrix': {'top_marked': [False, True], 'bottom_marked': [False, True],
                           'movement': ['all', 'partial', 'none'], 'boundary': ['Filter', 'Join', 'Map']},
                'required_observations': ['per-definition flag identity', 'only marked definitions move',
                                          'unmarked erroring expressions retain demand', 'outer-join null preservation']}}
