"""Quality acceptance builders and independent result models.

Rendered statements, fixture rows, plans and native result payloads must remain
in the caller's anonymous buffers. This module does not execute or write them.
Semantic success alone never establishes an optimization improvement: run the
paired physical-AST checks and retain only their counters, hashes and verdicts.
Q01 covers COUNT and its integer-SUM mixed case; Q15 covers exact partition keys.
"""
from collections import Counter
import hashlib
import json
import re

import generate_and_verify as g

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'quality_window_physical_contracts_v2'


def _int(value):
    return g.typed('Int64', value)


def _mod(value, divisor):
    return g.call('Unwrap', g.op(value, '%', _int(divisor)))


def _source(*, empty=False):
    optional = lambda period: g.call('IF', g.op(_mod('id', period), '=', _int(0)),
        g.call('Nothing', 'Int64?'), g.call('Just', 'id'))
    return g.select(['id', g.alias(_mod('id', 3), 'p'),
                     g.alias(_mod('id', 5), 'o'), g.alias(optional(4), 'v'),
                     g.alias(optional(5), 'w'), g.alias(g.call('Nothing', 'Int64?'), 'z')],
                    't', where=g.op('id', '<', _int(0)) if empty else None)


def _frame(kind, direction, width, prior=False):
    end = '1 PRECEDING' if prior else '1 FOLLOWING'
    return kind.upper() + ' BETWEEN ' + str(width) + ' PRECEDING AND ' + end


def _counts(n, kind, direction, *, prior=False, mixed=False, ranked=False, width=2, empty=False):
    order = ('id' if kind == 'rows' else 'o') + ' ' + direction.upper()
    frame = _frame(kind, direction, width, prior)
    fields = ['id'] + [g.alias(g.over(g.call('COUNT', argument), 'p', order, frame), name)
                      for argument, name in [('*', 'c'), ('v', 'cv'), ('w', 'cw'), ('z', 'cz')]]
    if mixed:
        fields += [g.alias(g.over(g.call('SUM', 'id'), 'p', order, frame), 's')]
    if ranked:
        fields += [g.alias(g.over(g.call('RANK'), 'p', order), 'r')]
    return g.select(fields, g.derived(_source(empty=empty), 's'), order='id')


def _window_chain(same_keys=True, intervening=False, keyless=False):
    partition = None if keyless else 'p'
    first = g.select(['id', 'p', 'o', g.alias(g.over(g.call('ROW_NUMBER'), partition, 'id ASC'), 'a')],
                     g.derived(_source(), 's'))
    if intervening:
        # A retained filter is a negative control for the deliberately direct
        # adjacency proof; whether it stays between windows is checked in IR.
        first = g.select(['id', 'p', 'o', 'a'], g.derived(first, 'f'),
                         where=g.op('a', '>', g.typed('Uint64', 1)))
    second_partition = None if keyless else ('p' if same_keys else 'o')
    return g.select(['id', 'a', g.alias(g.over(g.call('ROW_NUMBER'), second_partition, 'id DESC'), 'b')],
                    g.derived(first, 'w'), order='id')


def _in_values(size, nullable=False, dependent=False):
    values = [g.op('id', '+', _int(i)) if dependent else _int(i * 2) for i in range(1, size + 1)]
    if nullable:
        values = [g.call('Just', value) for value in values]
        values[-1] = g.call('Nothing', 'Int64?')
    return values


def recipes(n):
    if not isinstance(n, int) or n < 8:
        raise ValueError('Quality checks require at least eight rows')
    result = {}
    for kind in ('rows', 'range'):
        for direction in ('asc', 'desc'):
            for prior in (False, True):
                label = 'q01_' + kind + '_' + direction + ('_prior' if prior else '_overlap')
                result[label] = _counts(n, kind, direction, prior=prior)
    result['q01_rows_wide'] = _counts(n, 'rows', 'asc', width=max(3, n // 3))
    result['q01_rows_mixed'] = _counts(n, 'rows', 'asc', mixed=True)
    result['q01_rows_ranked'] = _counts(n, 'rows', 'asc', ranked=True)
    result['q01_empty_control'] = _counts(n, 'rows', 'asc', empty=True)

    framed = _counts(n, 'rows', 'asc')
    result['q02_all_dead'] = g.select(['id'], g.derived(framed, 'w'), order='id')
    result['q02_partial'] = g.select(['id', 'cv'], g.derived(framed, 'w'), order='id')
    result['q02_all_live_control'] = framed
    result['q02_empty'] = g.select(['id'], g.derived(_counts(n, 'rows', 'asc', empty=True), 'w'))

    for grouped in (False, True):
        for variant in ('shared', 'different', 'ordinary', 'all_null', 'empty'):
            source = _source(empty=variant == 'empty')
            argument = 'z' if variant == 'all_null' else 'v'
            fields = [g.alias(g.call('COUNT', 'DISTINCT ' + argument), 'c'),
                      g.alias(g.call('SUM', 'DISTINCT ' + argument), 's')]
            if variant == 'different':
                fields.append(g.alias(g.call('COUNT', 'DISTINCT w'), 'd'))
            if variant == 'ordinary':
                fields.append(g.alias(g.call('COUNT', '*'), 'r'))
            if grouped:
                fields.append('p')
            result['q10_' + ('grouped_' if grouped else 'scalar_') + variant] = g.select(
                fields, g.derived(source, 'a'), group='p' if grouped else None)

    for suffix, same, intervening, keyless in [
            ('same_partition', True, False, False), ('different_partition', False, False, False),
            ('intervening_filter', True, True, False), ('keyless_control', True, False, True)]:
        result['q15_' + suffix] = _window_chain(same, intervening, keyless)
    for suffix in ('disabled_control', 'override_enabled', 'override_disabled'):
        result['q15_' + suffix] = _window_chain()

    for ansi in (True, False):
        for shape in ('tuple', 'list'):
            for size in (5, 6, 17):
                for nullable in (False, True):
                    label = 'q22_' + ('ansi' if ansi else 'legacy') + '_' + shape + '_' + str(size)
                    label += '_nullable' if nullable else '_required'
                    items = _in_values(size, nullable)
                    collection = '(' + ', '.join(items) + ')' if shape == 'tuple' else g.call('AsList', *items)
                    needle = 'v' if nullable else 'id'
                    result[label] = g.select(['id', g.alias(g.op(needle, 'IN', collection), 'member')],
                                             g.derived(_source(), 's'), order='id')
    result['q22_row_dependent_control'] = g.select(['id', g.alias(g.op('id', 'IN',
        g.call('AsList', *_in_values(6, dependent=True))), 'member')], 't', order='id')

    for case in ('parallel', 'self_read', 'row_store_control'):
        table = 'qdst' if case == 'self_read' else 'qsrc'
        value = g.op('v', '+', _int(1)) if case == 'self_read' else 'v'
        result['q13_' + case] = 'UPSERT INTO qdst ' + g.select(['id', g.alias(value, 'v')], table)
    return result


def schema_and_seeds(n, case):
    if not case.startswith('q13_'):
        return g.schema_and_seeds(n, case)
    definitions = ['id Int64 NOT NULL', 'v Int64', 'PRIMARY KEY (id)']
    create = lambda table, column: 'CREATE TABLE ' + table + ' (' + ', '.join(definitions) + ')' + (
        ' WITH (STORE = COLUMN, PARTITION_COUNT = 4)' if column else '') + ';'
    schema = create('qsrc', True) + create('qdst', case != 'q13_row_store_control')
    values = g.combine([g.select([g.alias(_int(i), 'id'), g.alias(_int(i * 3), 'v')])
                        for i in range(1, n + 1)])
    seeds = ['UPSERT INTO qsrc ' + values]
    if case == 'q13_self_read':
        seeds.append('UPSERT INTO qdst ' + values)
    return schema, seeds


def validation_queries(case, n):
    return [g.select(['id', 'v'], 'qdst', order='id')] if case.startswith('q13_') else []


def metadata(case, n):
    config = {'EnableFallbackToYqlOptimizer': False}
    pragmas = [('ydb.WindowFunctionsV2', 'true')]
    flags = []
    nodes = 1
    if case.startswith('q13_'):
        config.update(EnableOlapSink=True, AllowOlapDataQuery=True)
        nodes = 4
    if case.startswith('q10_'):
        config.update(EnableQueryServiceSpilling=True, EnableSpillingInHashJoinShuffleConnections=True,
                      EnableSpillingNodes='All', DqHashOperatorsUseBlocks=False)
    if case.startswith('q15_'):
        config['DefaultEnableShuffleElimination'] = case not in ('q15_disabled_control', 'q15_override_enabled')
        if case in ('q15_override_enabled', 'q15_override_disabled'):
            pragmas += [('ydb.OptShuffleElimination', 'true' if case == 'q15_override_enabled' else 'false')]
        nodes = 4
    if case.startswith('q22_'):
        flags = ['DisableAnsiInForEmptyOrNullableItems' if '_legacy_' in case
                 else 'AnsiInForEmptyOrNullableItems']
    return {'generator_version': GENERATOR_VERSION, 'table_service_overrides': config,
            'pragmas': pragmas, 'flag_pragmas': flags, 'node_count': nodes,
            'quality_requires_separate_plan_evidence': True,
            'peak_native_concurrency': 1}


def _rows(n):
    return [{'id': i, 'p': i % 3, 'o': i % 5,
             'v': None if i % 4 == 0 else i, 'w': None if i % 5 == 0 else i}
            for i in range(1, n + 1)]


def _count_expected(case, n):
    if 'empty' in case:
        return []
    kind = 'range' if '_range_' in case else 'rows'
    descending = '_desc_' in case
    prior = '_prior' in case
    width = max(3, n // 3) if '_wide' in case else 2
    source = _rows(n)
    result = []
    for row in source:
        partition = [item for item in source if item['p'] == row['p']]
        if kind == 'rows':
            partition.sort(key=lambda item: item['id'], reverse=descending)
            position = next(index for index, item in enumerate(partition) if item['id'] == row['id'])
            lower, upper = max(0, position - width), min(len(partition) - 1, position + (-1 if prior else 1))
            members = partition[lower:upper + 1] if lower <= upper else []
        else:
            direction = -1 if descending else 1
            position = direction * row['o']
            lower, upper = position - width, position + (-1 if prior else 1)
            members = [item for item in partition if lower <= direction * item['o'] <= upper]
        output = {'id': row['id'], 'c': len(members), 'cv': sum(item['v'] is not None for item in members),
                  'cw': sum(item['w'] is not None for item in members), 'cz': 0}
        if '_mixed' in case:
            output['s'] = sum(item['id'] for item in members) if members else None
        if '_ranked' in case:
            output['r'] = position + 1
        result.append(output)
    return result


def expected_rows(case, n):
    if case.startswith('q01_'):
        return _count_expected(case, n)
    if case.startswith('q02_'):
        rows = _count_expected(case, n)
        names = ['id'] if case in ('q02_all_dead', 'q02_empty') else (
            ['id', 'cv'] if case == 'q02_partial' else ['id', 'c', 'cv', 'cw', 'cz'])
        return [{name: row[name] for name in names} for row in rows]
    if case.startswith('q13_'):
        return [{'id': i, 'v': i * 3 + (case == 'q13_self_read')} for i in range(1, n + 1)]
    if case.startswith('q10_'):
        source = [] if case.endswith('_empty') else _rows(n)
        grouped = '_grouped_' in case
        keys = sorted({row['p'] for row in source}) if grouped else [None]
        result = []
        for key in keys:
            members = [row for row in source if not grouped or row['p'] == key]
            values = set() if case.endswith('_all_null') else {row['v'] for row in members if row['v'] is not None}
            item = {'c': len(values), 's': sum(values) if values else None}
            if case.endswith('_different'):
                item['d'] = len({row['w'] for row in members if row['w'] is not None})
            if case.endswith('_ordinary'):
                item['r'] = len(members)
            if grouped:
                item['p'] = key
            result.append(item)
        return result
    if case.startswith('q15_'):
        rows = _rows(n)
        keyless = case == 'q15_keyless_control'
        for row in rows:
            row['a'] = sum(item['id'] <= row['id'] and (keyless or item['p'] == row['p']) for item in rows)
        if case == 'q15_intervening_filter':
            rows = [row for row in rows if row['a'] > 1]
        key = 'o' if case == 'q15_different_partition' else 'p'
        return [{'id': row['id'], 'a': row['a'],
                 'b': sum(item['id'] >= row['id'] and (keyless or item[key] == row[key]) for item in rows)}
                for row in rows]
    if case == 'q22_row_dependent_control':
        return [{'id': i, 'member': False} for i in range(1, n + 1)]
    if case.startswith('q22_'):
        parts = case.split('_')
        size, nullable, ansi = int(parts[3]), parts[4] == 'nullable', parts[1] == 'ansi'
        candidates = {i * 2 for i in range(1, size + (0 if nullable else 1))}
        result = []
        for row in _rows(n):
            needle = row['v'] if nullable else row['id']
            member = None if needle is None else (True if needle in candidates else (None if nullable and ansi else False))
            result.append({'id': row['id'], 'member': member})
        return result
    raise ValueError('Unknown quality case')


def check_rows(case, rows, n):
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values)
    return {'semantic_oracle_pass': canonical(rows) == canonical(expected_rows(case, n)),
            'independent_multiset_oracle': True, 'quality_improvement_verified': False}


def check_validation_results(case, result_sets, n):
    if len(result_sets) != 1:
        return {'semantic_oracle_pass': False, 'validation_result_set_count_valid': False}
    return check_rows(case, result_sets[0], n)


def _parse_ast(text):
    """Parse only a balanced S-expression AST, never a log with embedded plans."""
    tokens = re.findall(r'\'?"(?:\\.|[^"\\])*"|[()]|[^\s()]+', text)
    stack, roots = [], []
    for index, token in enumerate(tokens):
        if token == "'":
            if index + 1 >= len(tokens) or tokens[index + 1] != '(':
                raise ValueError('Malformed AST list quotation')
            continue
        if token == '(':
            node = []
            (stack[-1] if stack else roots).append(node)
            stack.append(node)
        elif token == ')':
            if not stack:
                raise ValueError('Unbalanced physical AST')
            stack.pop()
        else:
            if not stack:
                raise ValueError('Non-AST input is not accepted')
            stack[-1].append(token)
    if stack or len(roots) != 1:
        raise ValueError('Expected exactly one complete physical AST')
    return roots[0]


def parse_physical_graph(text):
    """Return (resolve_reference, reachable_nodes), strictly for in-memory use.

    The returned graph contains raw plan material; callers must never persist
    or print it. Follow the public expression-node child indices and export
    only approved counters, hashes and Boolean shape observations.
    """
    root = _parse_ast(text)
    bindings, returns = {}, []
    todo = [root]
    while todo:
        item = todo.pop()
        if not isinstance(item, list):
            continue
        if item and item[0] == 'let' and len(item) == 3 and isinstance(item[1], str):
            if item[1] in bindings:
                raise ValueError('Ambiguous repeated binding')
            bindings[item[1]] = item[2]
        elif item and item[0] == 'return' and len(item) == 2:
            returns.append(item[1])
        else:
            todo.extend(child for child in item if isinstance(child, list))
    seen, nodes = set(), []
    def resolve(node):
        followed = set()
        while isinstance(node, str) and node in bindings:
            if node in followed:
                raise ValueError('Cyclic physical AST binding')
            followed.add(node)
            node = bindings[node]
        return node
    todo = returns or [root]
    while todo:
        node = resolve(todo.pop())
        if not isinstance(node, list) or id(node) in seen:
            continue
        seen.add(id(node))
        nodes.append(node)
        # A list beginning with a binding reference is an expression list, not
        # a callable. Its first reference is an edge too (notably stage Inputs).
        callable_head = node and isinstance(node[0], str) and not node[0].startswith(('$', "'", '"'))
        todo.extend(node[1:] if callable_head else node)
    if not any(node and node[0] == 'DqPhyStage' for node in nodes):
        raise ValueError('Final physical stages are absent')
    return resolve, nodes


def physical_ast_summary(text):
    """Return safe graph counters only; quoted strings never become callables.

    Let references are followed from return roots and each DAG node counted
    once. Unknown documents fail closed instead of treating zero as success.
    The caller must supply the final physical AST, not an explain-plan string.
    """
    resolve, nodes = parse_physical_graph(text)
    counters, flags, sink_edges, switch_outputs, row_read_settings = Counter(), [], [], [], []
    for node in nodes:
        if node and isinstance(node[0], str):
            name = node[0]
            if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
                counters[name] += 1
            if name == 'KqpTableSinkSettings' and len(node) >= 6:
                clean = lambda item: str(resolve(item)).strip("'\"")
                flags.append({'inconsistent': clean(node[2]), 'stream': clean(node[5])})
            if name == 'Switch' and len(node) >= 5 and (len(node) - 3) % 2 == 0:
                switch_outputs.append((len(node) - 3) // 2)
            if name == 'KqpRowsSourceSettings' and len(node) > 3:
                settings = resolve(node[3])
                if not isinstance(settings, list):
                    raise ValueError('Unknown row-read settings shape')
                names = []
                for setting in settings:
                    setting = resolve(setting)
                    if isinstance(setting, list) and setting:
                        name_value = resolve(setting[0])
                        if isinstance(name_value, str):
                            setting_name = name_value.strip("'\"")
                            if setting_name in ('ItemsLimit', 'Sorted', 'Reverse'):
                                names.append(setting_name)
                row_read_settings.append(sorted(names))
            if name == 'DqPhyStage' and len(node) > 4:
                outputs, inputs = resolve(node[4]), resolve(node[1])
                if isinstance(outputs, list) and isinstance(inputs, list):
                    sinks = [resolve(output) for output in outputs]
                    if any(isinstance(sink, list) and sink and sink[0] == 'DqSink' for sink in sinks):
                        edge_names = []
                        for connection in inputs:
                            connection = resolve(connection)
                            if not isinstance(connection, list) or not connection or not isinstance(connection[0], str):
                                raise ValueError('Unrecognized sink input connection')
                            edge_names.append(connection[0])
                        sink_edges.append(edge_names)
    relevant = ('DqPhyStage', 'Fold', 'Fold1', 'WinFrame', 'WinFramesCollector', 'QueueCreate',
                'DqCnHashShuffle', 'DqCnMap', 'DqCnUnionAll', 'Contains', 'DictFromKeys', 'ToDict',
                'SqueezeToDict', 'SqlIn', 'KqpTableSinkSettings', 'Switch', 'KqpRowsSourceSettings',
                'ChainMap', 'Chain1Map', 'WideChainMap', 'WideChain1Map', 'KqpOlapJsonValue',
                'KqpOlapJsonExists', 'KqpOlapApply', 'KqpOlapFilter')
    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
            'callables': {name: counters[name] for name in relevant}, 'sink_flags': flags,
            'sink_incoming_connections': sink_edges,
            'switch_output_counts': sorted(switch_outputs), 'row_read_setting_names': row_read_settings,
            'physical_ast_parsed': True,
            # Comparison callable heads are symbols, outside the identifier-only counter.
            'scalar_equality_dag_nodes': sum(bool(node) and len(node) == 3 and node[0] == '==' for node in nodes)}


def _window_frame_fold_state(text):
    """Inspect only concrete frame-fold state nodes; never guess local aliases."""
    result = {'version': 'window_frame_fold_state_v1', 'complete': False, 'folds': []}
    try:
        resolve, nodes = parse_physical_graph(text)
        def head(node):
            return node[0] if isinstance(node, list) and node and isinstance(node[0], str) else None
        def atom(node):
            node = resolve(node)
            if not isinstance(node, str) or node.startswith('$'):
                raise ValueError('Unresolved field atom')
            node = node[1:] if node.startswith("'") else node
            value = json.loads(node) if node.startswith('"') else node
            if not isinstance(value, str):
                raise ValueError('Invalid field atom')
            return value
        def one_lambda(node):
            node = resolve(node)
            if head(node) != 'lambda' or len(node) != 3:
                raise ValueError('Unsupported frame lambda')
            args, body = resolve(node[1]), resolve(node[2])
            if (not isinstance(args, list) or any(not isinstance(arg, str) or not arg.startswith('$') for arg in args)
                    or len(set(args)) != len(args) or any(resolve(arg) != arg for arg in args)):
                raise ValueError('Unresolved frame arguments')
            return args, body
        def state_fields(body):
            if head(body) != 'AsStruct':
                raise ValueError('Unresolved frame state')
            fields = {}
            for pair in body[1:]:
                pair = resolve(pair)
                if not isinstance(pair, list) or len(pair) != 2:
                    raise ValueError('Invalid frame state field')
                name = atom(pair[0])
                if name in fields:
                    raise ValueError('Duplicate frame state field')
                fields[name] = resolve(pair[1])
            return fields
        def member(node, argument):
            node = resolve(node)
            if head(node) != 'Member' or len(node) != 3 or resolve(node[1]) != argument:
                return None
            return atom(node[2])
        collectors = [node for node in nodes if head(node) == 'WinFramesCollector']
        descriptors = []
        for fold in (node for node in nodes if head(node) == 'Fold1'):
            if len(fold) != 4:
                raise ValueError('Unsupported frame fold')
            frame = resolve(fold[1])
            if head(frame) in ('Map', 'OrderedMap') and len(frame) == 3:
                args, body = one_lambda(frame[2])
                if len(args) != 1 or body != args[0]:
                    raise ValueError('Nonidentity frame map')
                frame = resolve(frame[1])
            if head(frame) != 'WinFrame' or len(frame) != 7:
                raise ValueError('Unresolved frame source')
            queue = resolve(frame[1])
            if head(queue) != 'QueueCreate':
                raise ValueError('Unresolved frame queue')
            matched = [node for node in collectors if len(node) == 4 and resolve(node[2]) is queue]
            init_args, init_body = one_lambda(fold[2])
            update_args, update_body = one_lambda(fold[3])
            initial, updated = state_fields(init_body), state_fields(update_body)
            optional_counts = 0
            if len(init_args) == 1 and len(update_args) == 2:
                for name in initial.keys() & updated.keys():
                    first, later = initial[name], updated[name]
                    if head(first) != 'AggrCountInit' or len(first) != 2 or head(later) != 'AggrCountUpdate' or len(later) != 3:
                        continue
                    field = member(first[1], init_args[0])
                    if field is not None and field == member(later[1], update_args[0]) and member(later[2], update_args[1]) == name:
                        optional_counts += 1
            descriptors.append({'init_fields': len(initial), 'update_fields': len(updated),
                                'state_fields_match': initial.keys() == updated.keys(),
                                'init_arity': len(init_args), 'update_arity': len(update_args),
                                'optional_count_fields': optional_counts,
                                'collector_queue_bound': len(matched) == 1})
        result.update(complete=True, folds=descriptors)
    except (ValueError, TypeError, IndexError, KeyError, RecursionError) as error:
        result['shape_rejection'] = type(error).__name__
    return result


def _retained_optional_count_state(summary):
    if not isinstance(summary, dict) or summary.get('complete') is not True:
        return False
    folds = summary.get('folds')
    return (isinstance(folds, list) and len(folds) == 1 and folds[0] == {
        'init_fields': 1, 'update_fields': 1, 'state_fields_match': True,
        'init_arity': 1, 'update_arity': 2, 'optional_count_fields': 1,
        'collector_queue_bound': True})


def check_physical_ast(case, text, baseline=None):
    summary = physical_ast_summary(text)
    counts = summary['callables']
    assertions = {}
    if case.startswith('q01_') and case != 'q01_empty_control':
        assertions = {'frame_collection_present': counts['WinFramesCollector'] > 0,
                      'count_has_no_frame_fold': counts['Fold'] + counts['Fold1'] == 0,
                      'boundary_accesses_present': counts['WinFrame'] >= 2}
    elif case == 'q02_partial':
        summary['window_frame_fold_state'] = _window_frame_fold_state(text)
        assertions = {'retained_window_pipeline_present': counts['QueueCreate'] > 0 and counts['WinFramesCollector'] > 0,
                      'only_live_optional_count_state_retained': _retained_optional_count_state(summary['window_frame_fold_state'])}
    elif case == 'q02_all_dead':
        assertions = {'unused_frame_state_removed': counts['QueueCreate'] == 0 and counts['WinFramesCollector'] == 0}
    elif case.startswith('q10_') and not case.endswith(('_empty', '_all_null')):
        # The isolated recipes have only the expansion's one shared input. A
        # one-branch expansion removes Switch entirely; extra DISTINCT inputs
        # or an ordinary-aggregate branch each require another output.
        outputs = 2 if case.endswith(('_different', '_ordinary')) else 1
        assertions = {'one_branch_per_distinct_argument_and_ordinary_group':
                      summary['switch_output_counts'] == ([outputs] if outputs > 1 else [])}
    elif case.startswith('q15_'):
        expected = {'q15_same_partition': 1, 'q15_different_partition': 2,
                    'q15_keyless_control': 0, 'q15_intervening_filter': 2,
                    'q15_disabled_control': 2, 'q15_override_enabled': 1,
                    'q15_override_disabled': 2}[case]
        assertions = {'expected_shuffle_count': counts['DqCnHashShuffle'] == expected}
    elif case.startswith('q13_'):
        expected = case == 'q13_parallel'
        assertions = {'one_target_sink': len(summary['sink_flags']) == 1,
                      'expected_stream_flag': all(flag == {'stream': str(expected).lower(), 'inconsistent': 'false'}
                                                  for flag in summary['sink_flags']),
                      'expected_sink_input_connection': summary['sink_incoming_connections'] == [
                          ['DqCnMap' if expected else 'DqCnUnionAll']]}
        summary['sink_incoming_connection_checked'] = True
    elif case.startswith('q22_') and case != 'q22_row_dependent_control':
        if int(case.split('_')[3]) > 5:
            assertions = {'dictionary_membership_present': counts['Contains'] > 0,
                          'high_level_membership_lowered': counts['SqlIn'] == 0}
        elif int(case.split('_')[3]) == 5:
            assertions = {'small_collection_membership_lowered': counts['SqlIn'] == 0,
                          'small_collection_comparison_present': summary['scalar_equality_dag_nodes'] > 0}
            summary['paired_boundary_preservation_required'] = True
    if baseline is not None:
        if isinstance(baseline, str):
            before = physical_ast_summary(baseline)
            if case == 'q02_partial':
                before['window_frame_fold_state'] = _window_frame_fold_state(baseline)
        elif isinstance(baseline, dict):
            before = baseline
            if (before.get('physical_ast_parsed') is not True
                or not re.fullmatch('[0-9a-f]{64}', before.get('physical_ast_sha256', ''))
                or not isinstance(before.get('callables'), dict)
                or set(counts) - set(before['callables'])
                or any(type(value) is not int or value < 0 for value in before['callables'].values())):
                raise ValueError('Baseline AST summary is not validated counter evidence')
        else:
            raise ValueError('Expected a baseline AST or a validated summary')
        summary['baseline_physical_ast_sha256'] = before['physical_ast_sha256']
        summary['baseline_callables'] = before['callables']
        for key in ('sink_incoming_connections', 'switch_output_counts', 'row_read_setting_names'):
            if key in before:
                summary['baseline_' + key] = before[key]
        if case == 'q02_partial':
            previous = before.get('window_frame_fold_state', {})
            folds = previous.get('folds', []) if isinstance(previous, dict) else []
            assertions['baseline_dead_accumulator_fields_removed'] = (
                isinstance(previous, dict) and previous.get('complete') is True
                and isinstance(folds, list) and len(folds) == 1 and isinstance(folds[0], dict)
                and type(folds[0].get('init_fields')) is int and folds[0]['init_fields'] > 1
                and folds[0].get('init_fields') == folds[0].get('update_fields')
                and folds[0].get('state_fields_match') is True and folds[0].get('collector_queue_bound') is True)
        if case.startswith('q22_') and 'small_collection_comparison_present' in assertions:
            assertions['small_collection_strategy_preserved'] = (
                type(before.get('scalar_equality_dag_nodes')) is int
                and before['scalar_equality_dag_nodes'] == summary['scalar_equality_dag_nodes']
                and before['callables'] == counts)
        if case.startswith('q01_') and 'count_has_no_frame_fold' in assertions:
            assertions['baseline_frame_fold_present'] = before['callables']['Fold'] + before['callables']['Fold1'] > 0
        if case == 'q15_same_partition':
            assertions['baseline_redundant_shuffle_present'] = before['callables']['DqCnHashShuffle'] > counts['DqCnHashShuffle']
        if case.startswith('q10_') and not case.endswith(('_empty', '_all_null')):
            assertions['baseline_has_more_shared_outputs'] = (
                'switch_output_counts' in before
                and sum(before['switch_output_counts']) > sum(summary['switch_output_counts']))
        if case.startswith('q22_') and 'dictionary_membership_present' in assertions:
            assertions['membership_strategy_changed'] = before['callables']['Contains'] < counts['Contains']
    summary['structural_assertions'] = assertions
    summary['structural_oracle_pass'] = bool(assertions) and all(assertions.values())
    summary['quality_improvement_verified'] = False
    return summary
