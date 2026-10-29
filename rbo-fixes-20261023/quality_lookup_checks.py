"""Compositional lookup acceptance builders; rendered material stays in RAM.

Independent relational models establish semantics. Physical graph contracts
establish target reachability and structural work; neither a successful query
nor one final plan establishes an improvement over the pinned baseline. There
is deliberately no execution, printing or persistence entry point here.
"""
from collections import Counter
import hashlib
import json
import re

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'quality_lookup_independent_models_v1'

_VARIANTS = {
    'q03': ('uncovered_residual', 'nullable_primary_key', 'empty_result',
            'covered_control', 'disabled_control'),
    'q04': ('ascending', 'descending', 'nullable_order', 'unbounded_control',
            'direction_mismatch_control', 'nonmatching_index_control', 'disabled_control'),
    'q06': ('complete_primary_key', 'nullable_key', 'partial_primary_control', 'disabled_control'),
    'q07': ('exact_key_control', 'optional_key_control', 'mixed_key', 'mixed_key_large_right',
            'disabled_lookup', 'disabled_lookup_large_right', 'null_safe', 'residual_filter',
            'left_outer_mixed', 'nested_fallback', 'automatic_mixed'),
    'q17': ('implicit_primary_key', 'nullable_primary_key', 'declared_cover_control',
            'uncovered_control', 'disabled_control'),
    'q23': ('dead_probe_payload', 'selected_probe_control', 'residual_key_control',
            'prefix_equality', 'left_outer'),
    'q24': ('range_only_field', 'selected_key_control', 'residual_field_control',
            'empty_payload', 'duplicate_alias_control'),
    'q29': ('primitive_residual', 'optional_residual', 'arithmetic_control',
            'guard_control', 'no_range_control'),
}
CASES = tuple(f'{finding}_{variant}' for finding, variants in _VARIANTS.items() for variant in variants)
_DISTRIBUTED_CASES = frozenset(('q07_mixed_key', 'q07_mixed_key_large_right',
                              'q07_disabled_lookup_large_right', 'q07_left_outer_mixed', 'q07_nested_fallback'))


def _int(value, type_name='Int64'):
    return g.typed(type_name, value)


def _optional(value, type_name='Int64'):
    return g.call('Nothing', type_name + '?') if value is None else g.call('Just', g.typed(type_name, value))


def _and(*parts):
    result = parts[0]
    for part in parts[1:]:
        result = g.op(result, 'AND', part)
    return result


def _case(case):
    if case not in CASES:
        raise ValueError('Unknown lookup quality case')
    return case.split('_', 1)


def _right_rows(n, case):
    nullable_pk = case in ('q03_nullable_primary_key', 'q17_nullable_primary_key', 'q06_nullable_key', 'q07_null_safe')
    nullable_value = case in ('q04_nullable_order', 'q07_null_safe', 'q29_optional_residual')
    return [{'id': (i + 1) // 2, 'sid': None if nullable_pk and i % 5 == 0 else i % 2,
             'k': None if nullable_value and i % 4 == 0 else i % 3,
             'v': None if case == 'q29_optional_residual' and i % 4 == 0 else i % 5,
             'payload': 'p' + str(i) + ':' + chr(97 + i % 23) * (16 + i % 17)}
            for i in range(1, n + 1)]


def _left_rows(n, case):
    source = _right_rows(n, case)
    rows = [dict(row, lid=i) for i, row in enumerate(source, 1)]
    rows += [dict(source[0], lid=n + 1),
             {'lid': n + 2, 'id': n + 7, 'sid': 1, 'k': 7, 'v': 0, 'payload': ''},
             {'lid': n + 3, 'id': None, 'sid': None, 'k': None, 'v': None, 'payload': ''}]
    if case == 'q07_exact_key_control':
        rows = [row for row in rows if row['id'] is not None]
    if case == 'q07_residual_filter':
        for row in rows:
            if row['v'] is not None:
                row['v'] += -1 if row['lid'] % 2 else 1
    return rows


def _index_definition(case):
    finding, variant = _case(case)
    if finding == 'q03':
        return 'INDEX byk GLOBAL ON (k)' + (' COVER (v, payload)' if variant == 'covered_control' else '')
    if finding == 'q04':
        return ('INDEX byv GLOBAL ON (v) COVER (k, payload)' if variant == 'nonmatching_index_control'
                else 'INDEX byk GLOBAL ON (k) COVER (payload)')
    if finding == 'q06':
        return 'INDEX bykv GLOBAL ON (k, v) COVER (payload)'
    if finding == 'q17':
        return 'INDEX byk GLOBAL ON (k) COVER (payload)'
    return None


def schema_and_seeds(n, case):
    _case(case)
    right = ['id Int64 NOT NULL', 'sid Int64', 'k Int64', 'v Int64', 'payload Utf8', 'PRIMARY KEY (id, sid)']
    index = _index_definition(case)
    if index:
        right.append(index)
    mixed = 'mixed' in case or case == 'q07_nested_fallback'
    left_id_type = 'Int32' if mixed else 'Int64'
    left = ['lid Int64 NOT NULL', 'id ' + left_id_type + (' NOT NULL' if case == 'q07_exact_key_control' else ''),
            'sid Int64', 'k Int64', 'v Int64', 'PRIMARY KEY (lid)']
    schema = ''
    for table, fields in [('r', right), ('l', left)]:
        maximum = (n + 1) // 2 if table == 'r' else n + 3
        cuts = sorted({max(2, maximum * i // 4 + 1) for i in (1, 2, 3)})
        partitioning = ' WITH (PARTITION_AT_KEYS = (' + ', '.join(map(str, cuts)) + '))' if case in _DISTRIBUTED_CASES else ''
        schema += 'CREATE TABLE ' + table + ' (' + ', '.join(fields) + ')' + partitioning + ';'
    seeds = []
    for table, rows, fields in [('r', _right_rows(n, case), ('id', 'sid', 'k', 'v', 'payload')),
                                ('l', _left_rows(n, case), ('lid', 'id', 'sid', 'k', 'v'))]:
        arms = []
        for row in rows:
            values = []
            for field in fields:
                type_name = 'Utf8' if field == 'payload' else (left_id_type if table == 'l' and field == 'id' else 'Int64')
                required = field == 'lid' or field == 'id' and (table == 'r' or case == 'q07_exact_key_control')
                value = g.typed(type_name, row[field]) if required else _optional(row[field], type_name)
                values.append(g.alias(value, field))
            arms.append(g.select(values))
        seeds.append('UPSERT INTO ' + table + ' ' + g.combine(arms))
    return schema, seeds


def _q03(case):
    key = 2 if case == 'q03_nullable_primary_key' else 1
    minimum = 97 if case == 'q03_empty_result' else (0 if case == 'q03_nullable_primary_key' else 2)
    return g.select(['id', 'sid', 'payload'], 'r',
                    where=_and(g.op('k', '=', _int(key)), g.op('v', '>=', _int(minimum))))


def _q04(n, case):
    descending = case == 'q04_descending'
    direction, nulls = ('DESC', 'LAST') if descending else ('ASC', 'FIRST')
    terms = [field + ' ' + direction + ' NULLS ' + nulls for field in ('k', 'id', 'sid')]
    if case == 'q04_direction_mismatch_control':
        terms[1] = 'id DESC NULLS LAST'
    return g.select(['id', 'sid', 'k', 'payload'], 'r', order=', '.join(terms),
                    limit=None if case == 'q04_unbounded_control' else max(3, n // 2))


def _join_recipe(n, case):
    finding, variant = _case(case)
    join_keys = ['id', 'sid']
    if finding == 'q06':
        join_keys = ['k'] if variant == 'partial_primary_control' else ['id', 'sid', 'k']
    if finding == 'q17':
        join_keys = ['k']
    if case == 'q23_residual_key_control':
        join_keys.append('v')
    operation = 'IS NOT DISTINCT FROM' if case == 'q07_null_safe' else '='
    predicate = _and(*(g.op('l.' + key, operation, 'r.' + key) for key in join_keys))
    if case == 'q07_residual_filter':
        predicate = _and(predicate, g.op('l.v', '<', 'r.v'))
    fields = [g.alias('l.lid', 'lid'), g.alias('r.payload', 'payload')]
    if finding == 'q17' and variant != 'declared_cover_control':
        fields += [g.alias('r.id', 'id'), g.alias('r.sid', 'sid')]
    if case == 'q17_uncovered_control':
        fields.append(g.alias('r.v', 'v'))
    if case == 'q23_selected_probe_control':
        fields += [g.alias('l.id', 'probe_id'), g.alias('l.sid', 'probe_sid')]
    right = 'r AS r'
    where = None
    if case == 'q23_prefix_equality':
        where = g.op('id', '=', _int(1))
    if finding == 'q29':
        value = 'v'
        if variant == 'arithmetic_control':
            value = g.op('v', '+', _int(1))
        if variant == 'guard_control':
            value = g.call('Ensure', 'v', g.op('id', '>', _int(0)), g.quote('guard'))
        residual = g.op(value, '>=', _int(2))
        where = residual if variant == 'no_range_control' else _and(g.op('id', '>=', _int(2)), residual)
    if where is not None:
        right = g.derived(g.select(['id', 'sid', 'k', 'v', 'payload'], 'r', where=where), 'r')
    kind = 'LEFT' if case in ('q07_left_outer_mixed', 'q23_left_outer') else 'INNER'
    query = g.select(fields, g.join('l AS l', right, predicate, kind))
    if case == 'q07_nested_fallback':
        # An aggregate above the rejected lookup also exercises propagation of
        # stale distribution metadata after local fallback.
        query = g.select(['lid', g.alias(g.call('COUNT', '*'), 'c')], g.derived(query, 'j'), group='lid')
    return query


def _q24(n, case):
    predicate = _and(g.op('id', '>=', _int(2)), g.op('id', '<=', _int(max(2, n // 3))))
    fields = ['payload']
    if case == 'q24_selected_key_control':
        fields.append('id')
    elif case == 'q24_duplicate_alias_control':
        fields = [g.alias('id', 'a'), g.alias('id', 'b')]
    elif case == 'q24_empty_payload':
        fields = [g.alias(g.call('COUNT', '*'), 'c')]
    elif case == 'q24_residual_field_control':
        predicate = _and(predicate, g.op(g.op('v', '%', _int(2)), '=', _int(0)))
    return g.select(fields, 'r', where=predicate)


def recipes(n):
    if type(n) is not int or n < 8:
        raise ValueError('Lookup quality checks require at least eight rows')
    result = {}
    for case in CASES:
        if case.startswith('q03_'):
            result[case] = _q03(case)
        elif case.startswith('q04_'):
            result[case] = _q04(n, case)
        elif case.startswith('q24_'):
            result[case] = _q24(n, case)
        else:
            result[case] = _join_recipe(n, case)
    return result


def metadata(case, n):
    finding, variant = _case(case)
    config = {'EnableFallbackToYqlOptimizer': False, 'EnableQueryServiceSpilling': True,
              'EnableSpillingInHashJoinShuffleConnections': True, 'EnableSpillingNodes': 'All',
              'DqHashOperatorsUseBlocks': case in ('q07_null_safe', 'q07_residual_filter'),
              'EnableKqpDataQueryStreamIdxLookupJoin': not case.startswith('q07_disabled_lookup')}
    join_case = finding not in ('q03', 'q04', 'q24')
    auto_index = finding in ('q03', 'q04', 'q06', 'q17') and variant != 'disabled_control'
    pragmas = [('kikimr.CostBasedOptimizationLevel', '4' if join_case else '0'),
               ('kikimr.HashJoinMode', 'grace'),
               ('ydb.OptDisableAutoIndexSelection', 'false' if auto_index else 'true'),
               ('ydb.EnableAutoIndexSelectionForIndexLookupJoin', 'true' if auto_index else 'false')]
    if join_case:
        hints = ['JoinOrder(l r)']
        if case != 'q07_automatic_mixed':
            hints.append('JoinType(l r Lookup)')
        if case.endswith('_large_right'):
            hints += ['Rows(l # 300000)', 'Bytes(l # 10000000)', 'Rows(r # 300000)', 'Bytes(r # 10000000)']
        pragmas.append(('ydb.OptimizerHints', ' '.join(hints)))
    return {'generator_version': GENERATOR_VERSION, 'table_service_overrides': config,
            'pragmas': pragmas, 'node_count': 4 if case in _DISTRIBUTED_CASES else 1, 'peak_native_concurrency': 1,
            'table_partition_count': 4 if case in _DISTRIBUTED_CASES else 1,
            'quality_requires_separate_plan_evidence': True,
            'native_target_reachability_pending': True,
            'query_cache_coverage': False}


def _equal(left, right, null_safe=False):
    return left == right if null_safe else left is not None and right is not None and left == right


def expected_rows(case, n):
    finding, variant = _case(case)
    right = _right_rows(n, case)
    if finding == 'q03':
        key = 2 if variant == 'nullable_primary_key' else 1
        minimum = 97 if variant == 'empty_result' else (0 if variant == 'nullable_primary_key' else 2)
        return [{name: row[name] for name in ('id', 'sid', 'payload')} for row in right
                if row['k'] == key and row['v'] >= minimum]
    if finding == 'q04':
        key_value = lambda value: (value is not None, value if value is not None else 0)
        if variant == 'direction_mismatch_control':
            right.sort(key=lambda row: (key_value(row['k']), -row['id'], key_value(row['sid'])))
        else:
            right.sort(key=lambda row: tuple(key_value(row[name]) for name in ('k', 'id', 'sid')),
                       reverse=variant == 'descending')
        if variant != 'unbounded_control':
            right = right[:max(3, n // 2)]
        return [{name: row[name] for name in ('id', 'sid', 'k', 'payload')} for row in right]
    if finding == 'q24':
        selected = [row for row in right if 2 <= row['id'] <= max(2, n // 3)
                    and (variant != 'residual_field_control' or row['v'] % 2 == 0)]
        if variant == 'empty_payload':
            return [{'c': len(selected)}]
        if variant == 'duplicate_alias_control':
            return [{'a': row['id'], 'b': row['id']} for row in selected]
        fields = ('payload', 'id') if variant == 'selected_key_control' else ('payload',)
        return [{name: row[name] for name in fields} for row in selected]

    keys = ['id', 'sid']
    if finding == 'q06':
        keys = ['k'] if variant == 'partial_primary_control' else ['id', 'sid', 'k']
    if finding == 'q17':
        keys = ['k']
    if case == 'q23_residual_key_control':
        keys.append('v')
    if case == 'q23_prefix_equality':
        right = [row for row in right if row['id'] == 1]
    if finding == 'q29':
        right = [row for row in right if (variant == 'no_range_control' or row['id'] >= 2)
                 and row['v'] is not None and row['v'] + (variant == 'arithmetic_control') >= 2]
    result = []
    for left in _left_rows(n, case):
        matches = [row for row in right if all(_equal(left[key], row[key], case == 'q07_null_safe') for key in keys)
                   and (case != 'q07_residual_filter' or left['v'] is not None and row['v'] is not None and left['v'] < row['v'])]
        if not matches and case in ('q07_left_outer_mixed', 'q23_left_outer'):
            matches = [None]
        for row in matches:
            item = {'lid': left['lid'], 'payload': row['payload'] if row is not None else None}
            if finding == 'q17' and variant != 'declared_cover_control':
                item.update(id=row['id'], sid=row['sid'])
            if case == 'q17_uncovered_control':
                item['v'] = row['v']
            if case == 'q23_selected_probe_control':
                item.update(probe_id=left['id'], probe_sid=left['sid'])
            result.append(item)
    if case == 'q07_nested_fallback':
        counts = Counter(row['lid'] for row in result)
        return [{'lid': key, 'c': value} for key, value in counts.items()]
    return result


def check_rows(case, rows, n):
    canonical = lambda values: sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values)
    expected = expected_rows(case, n)
    ordered = case.startswith('q04_')
    return {'semantic_oracle_pass': rows == expected if ordered else canonical(rows) == canonical(expected),
            'independent_multiset_oracle': True, 'result_order_checked': ordered,
            'quality_improvement_verified': False}


_JOIN_CALLS = ('GraceJoinCore', 'GraceSelfJoinCore', 'MapJoinCore', 'BlockHashJoinCore', 'BlockMapJoinCore')
_CALLS = ('DqPhyStage', 'KqpRowsSourceSettings', 'KqpCnStreamLookup', 'KqpIndexLookupJoin',
          'DqCnHashShuffle', 'DqCnBroadcast', 'DqCnUnionAll', 'DqCnMap', 'Ensure',
          '==', '!=', '<', '<=', '>', '>=') + _JOIN_CALLS
_CONNECTIONS = ('DqCnHashShuffle', 'DqCnBroadcast', 'DqCnUnionAll', 'DqCnMap', 'KqpCnStreamLookup')


def _atom(node, resolve):
    node = resolve(node)
    if not isinstance(node, str):
        raise ValueError('Expected an AST atom')
    node = node[1:] if node.startswith("'") else node
    if node.startswith('"'):
        value = json.loads(node)
        if not isinstance(value, str):
            raise ValueError('Expected a string atom')
        return value
    return node


def _table_class(node, resolve):
    node = resolve(node)
    if not isinstance(node, list) or len(node) != 5 or node[0] != 'KqpTable':
        raise ValueError('Unknown table callable')
    path = _atom(node[1], resolve)
    if path.endswith('/l'):
        return 'left'
    if path.endswith('/r'):
        return 'right'
    if re.search(r'/r/(?:byk|bykv|byv)/indexImplTable$', path):
        return 'right_index'
    return 'other'


def _column_summary(node, resolve):
    node = resolve(node)
    if not isinstance(node, list):
        raise ValueError('Unknown column-list shape')
    names = [_atom(item, resolve) for item in node]
    return {'columns': len(names), 'unique_columns': len(set(names)),
            **{'has_' + name: name in names for name in ('id', 'sid', 'k', 'v', 'payload')}}


def _type_widths(node, resolve):
    node = resolve(node)
    if not isinstance(node, list) or len(node) != 2 or node[0] != 'ListType':
        raise ValueError('Unknown lookup input type')
    item = resolve(node[1])
    if not isinstance(item, list) or not item:
        raise ValueError('Unknown lookup item type')
    def fields(value):
        value = resolve(value)
        if not isinstance(value, list) or not value or value[0] != 'StructType':
            raise ValueError('Unknown lookup struct type')
        for field in value[1:]:
            field = resolve(field)
            if not isinstance(field, list) or len(field) != 2:
                raise ValueError('Unknown struct field shape')
            _atom(field[0], resolve)
        return len(value) - 1
    if item[0] == 'StructType':
        return {'join_payload': False, 'payload_fields': 0, 'key_fields': fields(item)}
    if item[0] != 'TupleType' or len(item) != 3:
        raise ValueError('Unknown lookup join tuple type')
    key = resolve(item[2])
    if not isinstance(key, list) or len(key) != 2 or key[0] != 'OptionalType':
        raise ValueError('Unknown lookup join key optional type')
    return {'join_payload': True, 'payload_fields': fields(item[1]), 'key_fields': fields(key[1])}


def _subgraph_calls(root, resolve):
    todo, seen, result = [root], set(), Counter()
    while todo:
        node = resolve(todo.pop())
        if not isinstance(node, list) or id(node) in seen:
            continue
        seen.add(id(node))
        callable_head = node and isinstance(node[0], str) and not node[0].startswith(('$', "'", '"'))
        if callable_head and node[0] in _CALLS:
            result[node[0]] += 1
        todo.extend(node[1:] if callable_head else node)
    return result


def physical_ast_summary(text):
    resolve, nodes = windows.parse_physical_graph(text)
    counts = Counter(node[0] for node in nodes if node and isinstance(node[0], str) and node[0] in _CALLS)
    reads, lookups, joins = [], [], []
    for node in nodes:
        if not node:
            continue
        if node[0] == 'KqpRowsSourceSettings':
            if len(node) not in (5, 6):
                raise ValueError('Unknown row source shape')
            descriptor = {'table_class': _table_class(node[1], resolve), **_column_summary(node[2], resolve)}
            settings = resolve(node[3])
            if not isinstance(settings, list):
                raise ValueError('Unknown row source settings')
            names = []
            for setting in settings:
                setting = resolve(setting)
                if not isinstance(setting, list) or not setting:
                    raise ValueError('Unknown row source setting')
                name = _atom(setting[0], resolve)
                if name in ('ItemsLimit', 'Sorted', 'Reverse'):
                    names.append(name)
            descriptor['settings'] = sorted(names)
            ranges = resolve(node[4])
            descriptor['range_expression_present'] = isinstance(ranges, list) and bool(ranges) and ranges[0] != 'Void'
            reads.append(descriptor)
        elif node[0] == 'KqpCnStreamLookup':
            if len(node) != 6:
                raise ValueError('Unknown stream lookup shape')
            lookups.append({'table_class': _table_class(node[2], resolve),
                            **_column_summary(node[3], resolve), **_type_widths(node[4], resolve)})
        elif node[0] == 'DqPhyStage' and len(node) >= 5:
            calls = _subgraph_calls(node[2], resolve)
            if any(calls[name] for name in _JOIN_CALLS):
                inputs = resolve(node[1])
                if not isinstance(inputs, list):
                    raise ValueError('Unknown join stage input list')
                connections = []
                for item in inputs:
                    item = resolve(item)
                    connections.append(item[0] if isinstance(item, list) and item and item[0] in _CONNECTIONS else 'other')
                joins.append({'input_connections': connections,
                              'join_callables': {name: calls[name] for name in _JOIN_CALLS}})
    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(), 'physical_ast_parsed': True,
            'lookup_summary_version': 1, 'callables': {name: counts[name] for name in _CALLS},
            'row_sources': sorted(reads, key=lambda item: json.dumps(item, sort_keys=True)),
            'stream_lookups': sorted(lookups, key=lambda item: json.dumps(item, sort_keys=True)),
            'hash_join_stages': joins, 'comparison_dag_nodes': sum(counts[name] for name in ('==', '!=', '<', '<=', '>', '>='))}


def _baseline_summary(value):
    if isinstance(value, str):
        return physical_ast_summary(value)
    if not isinstance(value, dict):
        raise ValueError('Expected baseline AST or sanitized counters')
    required = ('physical_ast_sha256', 'physical_ast_parsed', 'lookup_summary_version', 'callables',
                'row_sources', 'stream_lookups', 'hash_join_stages', 'comparison_dag_nodes')
    if (set(required) - set(value) or value['physical_ast_parsed'] is not True or value['lookup_summary_version'] != 1
        or not isinstance(value['physical_ast_sha256'], str)
        or not re.fullmatch('[0-9a-f]{64}', value['physical_ast_sha256'])
        or not isinstance(value['callables'], dict) or set(value['callables']) != set(_CALLS)
        or any(type(count) is not int or count < 0 for count in value['callables'].values())
        or type(value['comparison_dag_nodes']) is not int or value['comparison_dag_nodes'] < 0):
        raise ValueError('Invalid baseline lookup counter evidence')
    # A persisted baseline must contain only the same closed descriptor schema
    # the parser exports; arbitrary AST strings must never pass through here.
    for descriptors, lookup in [(value['row_sources'], False), (value['stream_lookups'], True)]:
        if not isinstance(descriptors, list):
            raise ValueError('Invalid baseline descriptor list')
        for item in descriptors:
            common = {'table_class', 'columns', 'unique_columns', 'has_id', 'has_sid', 'has_k', 'has_v', 'has_payload'}
            extra = {'join_payload', 'payload_fields', 'key_fields'} if lookup else {'settings', 'range_expression_present'}
            if (not isinstance(item, dict) or set(item) != common | extra
                or item['table_class'] not in ('left', 'right', 'right_index', 'other')
                or any(type(item[key]) is not int or item[key] < 0 for key in ('columns', 'unique_columns'))
                or any(type(item['has_' + key]) is not bool for key in ('id', 'sid', 'k', 'v', 'payload'))):
                raise ValueError('Invalid baseline source descriptor')
            if lookup:
                if type(item['join_payload']) is not bool or any(type(item[key]) is not int or item[key] < 0 for key in ('payload_fields', 'key_fields')):
                    raise ValueError('Invalid baseline lookup widths')
            elif (type(item['range_expression_present']) is not bool or not isinstance(item['settings'], list)
                  or any(setting not in ('ItemsLimit', 'Sorted', 'Reverse') for setting in item['settings'])):
                raise ValueError('Invalid baseline row-read settings')
    if not isinstance(value['hash_join_stages'], list):
        raise ValueError('Invalid baseline join stages')
    for item in value['hash_join_stages']:
        if (not isinstance(item, dict) or set(item) != {'input_connections', 'join_callables'}
            or not isinstance(item['input_connections'], list)
            or any(name not in _CONNECTIONS + ('other',) for name in item['input_connections'])
            or not isinstance(item['join_callables'], dict) or set(item['join_callables']) != set(_JOIN_CALLS)
            or any(type(count) is not int or count < 0 for count in item['join_callables'].values())):
            raise ValueError('Invalid baseline join descriptor')
    return {key: value[key] for key in required}


def check_physical_ast(case, text, baseline=None):
    finding, variant = _case(case)
    summary = physical_ast_summary(text)
    counts = summary['callables']
    reads, lookups = summary['row_sources'], summary['stream_lookups']
    index_reads = [item for item in reads if item['table_class'] == 'right_index']
    right_reads = [item for item in reads if item['table_class'] == 'right']
    index_lookups = [item for item in lookups if item['table_class'] == 'right_index']
    right_lookups = [item for item in lookups if item['table_class'] == 'right']
    join_lookups = [item for item in lookups if item['join_payload']]
    assertions = {}
    if finding == 'q03':
        if variant == 'disabled_control':
            assertions['disabled_auto_index_preserved'] = not index_reads and bool(right_reads)
        elif variant == 'covered_control':
            assertions['covered_index_uses_no_base_fetch'] = bool(index_reads) and not right_lookups
        else:
            assertions['bounded_index_and_base_fetch_present'] = bool(index_reads) and bool(right_lookups)
            assertions['index_does_not_read_uncovered_residual'] = bool(index_reads) and all(not item['has_v'] for item in index_reads)
    elif finding == 'q04':
        eligible = variant in ('ascending', 'descending', 'nullable_order')
        assertions['ordered_covering_index_eligibility_preserved'] = bool(index_reads) == eligible
        if eligible:
            assertions['storage_limit_and_direction_present'] = any('ItemsLimit' in item['settings'] and
                ('Reverse' if variant == 'descending' else 'Sorted') in item['settings'] for item in index_reads)
    elif finding == 'q06':
        partial = variant == 'partial_primary_control'
        assertions['complete_primary_probe_preferred'] = bool(index_lookups) == partial and bool(right_lookups) != partial
        assertions['lookup_lowered'] = counts['KqpIndexLookupJoin'] > 0
        if not partial:
            assertions['complete_primary_key_width'] = any(item['key_fields'] == 2 for item in right_lookups)
    elif finding == 'q07':
        eligible = variant in ('exact_key_control', 'optional_key_control')
        assertions['lookup_costing_matches_lowering'] = bool(join_lookups) == eligible
        if not eligible:
            assertions['supported_fallback_present'] = bool(summary['hash_join_stages'])
            if variant.endswith('_large_right'):
                assertions['large_right_not_broadcast'] = counts['DqCnBroadcast'] == 0 and counts['MapJoinCore'] == 0
            if ('mixed' in variant or variant == 'nested_fallback') and variant != 'automatic_mixed':
                assertions['mixed_hash_inputs_gathered'] = any(item['input_connections'] == ['DqCnUnionAll', 'DqCnUnionAll']
                    for item in summary['hash_join_stages'])
    elif finding == 'q17':
        eligible = variant not in ('uncovered_control', 'disabled_control')
        assertions['physical_index_coverage_used'] = bool(index_lookups) == eligible
        if eligible:
            assertions['single_covering_lookup'] = len(index_lookups) == 1 and not right_lookups
    elif finding == 'q23':
        expected_width = 3 if variant == 'selected_probe_control' else (2 if variant == 'residual_key_control' else 1)
        assertions['lookup_payload_shape_present'] = len(join_lookups) == 1
        assertions['required_payload_width'] = len(join_lookups) == 1 and join_lookups[0]['payload_fields'] == expected_width
    elif finding == 'q24':
        selected = variant in ('selected_key_control', 'duplicate_alias_control')
        assertions['single_base_read'] = len(right_reads) == 1
        assertions['range_key_liveness_respected'] = len(right_reads) == 1 and right_reads[0]['has_id'] == selected
        if variant == 'residual_field_control':
            assertions['residual_field_retained'] = len(right_reads) == 1 and right_reads[0]['has_v']
        if variant == 'empty_payload':
            assertions['zero_column_count_payload'] = len(right_reads) == 1 and right_reads[0]['columns'] == 0
    elif finding == 'q29':
        assertions['fetched_row_filter_target_present'] = bool(join_lookups)
        assertions['residual_comparison_present'] = summary['comparison_dag_nodes'] > 0
        if variant == 'guard_control':
            assertions['guard_preserved'] = counts['Ensure'] > 0
    if baseline is not None:
        before = _baseline_summary(baseline)
        summary['baseline_physical_ast_sha256'] = before['physical_ast_sha256']
        summary['baseline_comparison_dag_nodes'] = before['comparison_dag_nodes']
        summary['baseline_callables'] = before['callables']
        if finding == 'q03' and variant not in ('disabled_control', 'covered_control'):
            assertions['baseline_index_opportunity_absent'] = not any(item['table_class'] == 'right_index' for item in before['row_sources'])
        elif finding == 'q04' and variant in ('ascending', 'descending', 'nullable_order'):
            assertions['baseline_ordered_index_absent'] = not any(item['table_class'] == 'right_index' for item in before['row_sources'])
        elif finding == 'q06' and variant not in ('disabled_control', 'partial_primary_control'):
            assertions['baseline_incomplete_index_probe_replaced'] = any(item['table_class'] == 'right_index' for item in before['stream_lookups'])
        elif finding == 'q07' and variant.endswith('_large_right'):
            assertions['baseline_forced_broadcast_replaced'] = before['callables']['MapJoinCore'] > counts['MapJoinCore']
        elif finding == 'q17' and variant in ('implicit_primary_key', 'nullable_primary_key'):
            assertions['baseline_physical_coverage_missed'] = not any(item['table_class'] == 'right_index' for item in before['stream_lookups'])
        elif finding == 'q23' and variant in ('dead_probe_payload', 'prefix_equality', 'left_outer'):
            assertions['lookup_payload_reduced'] = sum(item['payload_fields'] for item in before['stream_lookups']) > sum(item['payload_fields'] for item in lookups)
        elif finding == 'q24' and variant in ('range_only_field', 'empty_payload', 'residual_field_control'):
            assertions['source_projection_reduced'] = sum(item['columns'] for item in before['row_sources']) > sum(item['columns'] for item in reads)
        elif finding == 'q29':
            if variant in ('primitive_residual', 'optional_residual'):
                assertions['duplicate_comparison_dag_nodes_removed'] = before['comparison_dag_nodes'] > summary['comparison_dag_nodes']
            else:
                assertions['unsafe_expression_structure_preserved'] = before['comparison_dag_nodes'] == summary['comparison_dag_nodes']
                if variant == 'guard_control':
                    assertions['guard_evaluation_sites_preserved'] = before['callables']['Ensure'] == counts['Ensure']
    summary['structural_assertions'] = assertions
    summary['structural_oracle_pass'] = bool(assertions) and all(assertions.values())
    summary['evaluation_count_verified'] = False
    summary['quality_improvement_verified'] = False
    return summary
