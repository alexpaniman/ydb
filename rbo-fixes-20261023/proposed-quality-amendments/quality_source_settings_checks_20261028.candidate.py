"""Q26 source-setting builders and paired, sanitized physical observations."""
from collections import Counter
import hashlib
import json
import re

import generate_and_verify as g
import quality_statistics_checks as statistics
from quality_window_checks import parse_physical_graph

CASE_SPECIFIC_SCHEMA = True
VERSION = 'q26_source_kind_controls_v1'
_SETTINGS = ('unset', 'zero', 'positive')
_ROW_SHAPES = ('ordinary', 'sorted_limit')
_COLUMN_SHAPES = ('ordinary', 'unsorted_limit', 'sorted_limit')
_SOURCE_CALLS = {'KqpRowsSourceSettings': ('row', 2, 3, 4),
                 'KqpSysViewSourceSettings': ('sysview', 2, 3, 4),
                 'KqpBlockReadOlapTableRanges': ('column', 3, 4, 2)}


def case_specs():
    result = {}
    for setting in _SETTINGS:
        for shape in _ROW_SHAPES:
            result['q26_' + setting + '_' + shape] = (setting, 'row', shape)
        result['q26_' + setting + '_sysview_control'] = (setting, 'sysview', 'ordinary')
        for shape in _COLUMN_SHAPES:
            result['q26_' + setting + '_column_' + shape + '_control'] = (setting, 'column', shape)
    return result


def _spec(case):
    return case_specs()[case]


def recipes(n):
    existing = statistics.recipes(n)
    result = {}
    for case, (_, kind, shape) in case_specs().items():
        if kind == 'row':
            result[case] = existing[case]
        elif kind == 'sysview':
            result[case] = g.select([g.alias(g.call('COUNT', '*'), 'c')], '`/Root/.sys/nodes`')
        else:
            result[case] = g.select(['v'] if shape == 'unsorted_limit' else ['id', 'v'], 'qc',
                order='id' if shape == 'sorted_limit' else None,
                limit=3 if shape.endswith('limit') else None)
    return result


def schema_and_seeds(n, case):
    _, kind, _ = _spec(case)
    if kind == 'row':
        return statistics.schema_and_seeds(n, case)
    if kind == 'sysview':
        # Keep the runner's schema request nonempty; the target reads only nodes.
        schema, _ = statistics.schema_and_seeds(n, 'q26_unset_ordinary')
        return schema, []
    fields = ['id Int64 NOT NULL', 'v Int64 NOT NULL', 'PRIMARY KEY (id)']
    schema = 'CREATE TABLE qc (' + ', '.join(fields) + ') PARTITION BY HASH(id)'
    schema += ' WITH (STORE = COLUMN, PARTITION_COUNT = 4);'
    values = [g.select([g.alias(g.typed('Int64', i), 'id'), g.alias(g.typed('Int64', 7), 'v')])
              for i in range(1, n + 1)]
    return schema, ['UPSERT INTO qc ' + g.combine(values)]


def metadata(case, n):
    setting, kind, _ = _spec(case)
    result = statistics.metadata('q26_' + setting + '_ordinary', n)
    result['generator_version'] = VERSION
    if kind == 'column':
        result['table_service_overrides'].update(EnableOlapSink=True, AllowOlapDataQuery=True,
                                                DisableOlapBlocksOnColumnsLimit=0)
    return result


def expected_rows(case, n):
    _, kind, shape = _spec(case)
    if kind == 'row':
        return statistics.expected_rows(case, n)
    if kind == 'sysview':
        return [{'c': 1}]
    if shape == 'unsorted_limit':
        # All projected values match; arbitrary storage order has one exact bag.
        return [{'v': 7} for _ in range(3)]
    return [{'id': i, 'v': 7} for i in range(1, (3 if shape == 'sorted_limit' else n) + 1)]


def check_rows(case, rows, n):
    _, kind, shape = _spec(case)
    ordered = kind == 'row' or kind == 'column' and shape == 'sorted_limit'
    expected = expected_rows(case, n)
    canonical = lambda values: Counter(json.dumps(row, sort_keys=True) for row in values)
    return {'semantic_oracle_pass': rows == expected if ordered else canonical(rows) == canonical(expected),
            'result_order_checked': ordered,
            'quality_improvement_verified': False}


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def _atom(node, resolve):
    node = resolve(node)
    if not isinstance(node, str):
        raise ValueError('Expected a source-setting atom')
    node = node[1:] if node.startswith("'") else node
    if node.startswith('"'):
        node = json.loads(node)
    if not isinstance(node, str):
        raise ValueError('Expected a source-setting string atom')
    return node


def _uint(node, resolve, typed=False):
    node = resolve(node)
    if typed:
        if not isinstance(node, list) or len(node) != 2 or node[0] != 'Uint64':
            raise ValueError('Unresolved storage limit')
        node = node[1]
    value = _atom(node, resolve)
    if not re.fullmatch(r'0|[1-9][0-9]*', value):
        raise ValueError('Invalid unsigned source setting')
    return int(value)


def _resolved(node, resolve, active=None):
    node = resolve(node)
    if not isinstance(node, list):
        return node
    active = set() if active is None else active
    if id(node) in active or len(active) > 256:
        raise ValueError('Cyclic or oversized setting expression')
    active.add(id(node))
    result = [_resolved(value, resolve, active) for value in node]
    active.remove(id(node))
    return result


def _descriptor(node, resolve):
    kind, columns_index, settings_index, _ = _SOURCE_CALLS[node[0]]
    expected_length = (5,) if kind == 'sysview' else (5, 6) if kind == 'row' else (7,)
    if len(node) not in expected_length:
        raise ValueError('Unknown source node shape')
    table = resolve(node[1])
    if not isinstance(table, list) or len(table) != 5 or table[0] != 'KqpTable':
        raise ValueError('Unknown source table identity')
    path, sysview = _atom(table[1], resolve), _atom(table[3], resolve)
    expected_path = {'row': '/Root/qs', 'column': '/Root/qc', 'sysview': '/Root/.sys/nodes'}[kind]
    if path != expected_path or sysview != ('nodes' if kind == 'sysview' else ''):
        raise ValueError('Source identity does not match the generated table')
    columns = resolve(node[columns_index])
    settings = resolve(node[settings_index])
    if not isinstance(columns, list) or not isinstance(settings, list):
        raise ValueError('Unresolved source projection or settings')
    columns = [_atom(column, resolve) for column in columns]
    if len(columns) != len(set(columns)):
        raise ValueError('Duplicate source columns')
    names, nonsequential, sequential = set(), [], []
    limit, sorted_read, reverse = None, False, False
    for setting in settings:
        setting = resolve(setting)
        if not isinstance(setting, list) or not setting:
            raise ValueError('Malformed source setting')
        name = _atom(setting[0], resolve)
        if name in names:
            raise ValueError('Duplicate source setting')
        names.add(name)
        if name == 'Sequential':
            if len(setting) != 2:
                raise ValueError('Malformed sequential source setting')
            sequential.append(_uint(setting[1], resolve))
            continue
        if name == 'ItemsLimit':
            if len(setting) != 2:
                raise ValueError('Malformed storage limit')
            limit = _uint(setting[1], resolve, typed=True)
        elif name in ('Sorted', 'Reverse'):
            if len(setting) != 1:
                raise ValueError('Malformed storage ordering')
            sorted_read |= name == 'Sorted'
            reverse |= name == 'Reverse'
        nonsequential.append(_resolved(setting, resolve))
    return {'kind': kind, 'source_callable': node[0], 'table_identity_checked': True,
            'columns_count': len(columns), 'columns_sha256': _digest(columns),
            'sequential': sequential, 'items_limit': limit,
            'sorted': sorted_read, 'reverse': reverse,
            'other_settings_sha256': _digest(nonsequential)}


def _expected_sequential(case, fixed):
    setting, kind, shape = _spec(case)
    if kind == 'column' and shape == 'unsorted_limit':
        return [1]
    if kind != 'row' or setting == 'unset' or not fixed:
        return []
    return [0 if setting == 'zero' else 4]


def _valid_descriptor(case, descriptor, fixed):
    _, kind, shape = _spec(case)
    if not isinstance(descriptor, dict):
        return False
    if set(descriptor) != {'kind', 'source_callable', 'table_identity_checked', 'columns_count',
                           'columns_sha256', 'sequential', 'items_limit', 'sorted', 'reverse',
                           'other_settings_sha256'}:
        return False
    names = {'row': 'KqpRowsSourceSettings', 'sysview': 'KqpSysViewSourceSettings',
             'column': 'KqpBlockReadOlapTableRanges'}
    sequential = descriptor.get('sequential')
    if (descriptor.get('kind') != kind or descriptor.get('source_callable') != names[kind]
        or descriptor.get('table_identity_checked') is not True
        or type(descriptor.get('columns_count')) is not int or descriptor['columns_count'] < 0
        or not all(isinstance(descriptor.get(field), str) and re.fullmatch('[0-9a-f]{64}', descriptor[field])
                   for field in ('columns_sha256', 'other_settings_sha256'))
        or not isinstance(sequential, list) or any(type(value) is not int or value < 0 for value in sequential)
        or sequential != _expected_sequential(case, fixed)
        or type(descriptor.get('sorted')) is not bool or type(descriptor.get('reverse')) is not bool
        or descriptor['reverse'] is not False):
        return False
    limit = descriptor.get('items_limit')
    if limit is not None and type(limit) is not int:
        return False
    if kind == 'column' and shape == 'unsorted_limit':
        return limit == 3 and descriptor['sorted'] is False
    if shape == 'sorted_limit':
        return limit == 3 and descriptor['sorted'] is True
    # Ordinary ORDER BY has no limit, so the row TopSort pushdown is ineligible.
    return limit is None and descriptor['sorted'] is False


def check_physical_ast(case, text):
    _spec(case)
    resolve, nodes = parse_physical_graph(text)
    sources = [_descriptor(node, resolve) for node in nodes
               if node and isinstance(node[0], str) and node[0] in _SOURCE_CALLS]
    callables = Counter(node[0] for node in nodes if node and isinstance(node[0], str)
                        and re.fullmatch('[A-Za-z][A-Za-z0-9_]*', node[0]))
    accepted = len(sources) == 1 and _valid_descriptor(case, sources[0], fixed=True)
    return {'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(), 'physical_ast_parsed': True,
            'q26_source_contract_version': VERSION, 'q26_sources': sources,
            'callables': dict(sorted(callables.items())),
            'quality_acceptance_pass': accepted, 'quality_improvement_verified': False}


def paired_acceptance(before, after):
    case = after.get('case')
    if case not in case_specs() or before.get('case') != case:
        return False
    descriptors = []
    for record, fixed in ((before, False), (after, True)):
        sources, callables = record.get('q26_sources'), record.get('callables')
        if (record.get('q26_source_contract_version') != VERSION or record.get('physical_ast_parsed') is not True
            or not isinstance(sources, list) or len(sources) != 1
            or not _valid_descriptor(case, sources[0], fixed)
            or not isinstance(callables, dict) or not callables
            or any(not isinstance(key, str) or type(value) is not int or value < 0
                   for key, value in callables.items())):
            return False
        descriptors.append({key: value for key, value in sources[0].items() if key != 'sequential'})
    return descriptors[0] == descriptors[1] and before['callables'] == after['callables']


def paired_improvement(before, after):
    case = after.get('case')
    if case not in case_specs():
        return False
    setting, kind, _ = _spec(case)
    return kind == 'row' and setting != 'unset' and paired_acceptance(before, after)
