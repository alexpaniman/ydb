"""Dormant bounded Q01 scaling cells; generated inputs remain in memory.

This selects the existing COUNT/SUM and scalar MIN/MAX window families. It
provides an independent linear-space row model and raw physical counters, not
resource admission, a target-only timer, or a complete acceptance matrix.
"""
from bisect import bisect_left, bisect_right
from collections import deque
import hashlib
import json
from pathlib import Path

import generate_and_verify as g
import quality_window_checks as windows

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q01_scaling_cells_v1'
WIDTHS = (16, 128, 512)
ROWS = (2048, 4096, 8192, 16384, 32768)
FAMILIES = ('rows_collectors', 'rows_minmax', 'range_minmax', 'range_mixed', 'range_collectors')


def specifications():
    return {'q01_scaling_' + family + '_w' + str(width):
            {'family': family, 'frame_width': width, 'partition_count': 3,
             'kind': 'range' if family.startswith('range_') else 'rows',
             'minmax': family.endswith(('minmax', 'mixed')),
             'collectors': family.endswith(('collectors', 'mixed'))}
            for family in FAMILIES for width in WIDTHS}


def cell(case, n):
    if type(n) is not int or n not in ROWS or case not in specifications():
        raise ValueError('unknown_q01_scaling_cell')
    return dict(specifications()[case], rows=n, case=case,
                generator_version=GENERATOR_VERSION)


def validate_sample(value):
    if (not isinstance(value, dict) or set(value) != {'case', 'rows', 'run_id', 'kind', 'index'}
            or value['kind'] not in ('pilot', 'warmup', 'measurement')
            or type(value['index']) is not int or not 0 <= value['index'] <= 4
            or value['kind'] == 'warmup' and value['index'] != 0
            or not isinstance(value['run_id'], str) or len(value['run_id']) != 32
            or any(c not in '0123456789abcdef' for c in value['run_id'])):
        raise ValueError('invalid_q01_sample_coordinates')
    return dict(value, cell=cell(value['case'], value['rows']))


def source_bindings():
    # Both builder and AST interpretation dependencies affect the observation.
    return {module.__name__: hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()
            for module in (g, windows)}


def _source():
    value = g.op(windows._mod('id', 97), '-', windows._int(48))
    optional = g.call('IF', g.op(windows._mod('id', 13), '<', windows._int(9)),
                      g.call('Nothing', 'Int64?'), g.call('Just', value))
    return g.select(['id', g.alias(windows._mod('id', 3), 'p'), g.alias(optional, 'v')], 't')


def recipes(n):
    if type(n) is not int or n not in ROWS:
        raise ValueError('unknown_q01_scaling_rows')
    result = {}
    for case, spec in specifications().items():
        frame = spec['kind'].upper() + ' BETWEEN ' + str(spec['frame_width']) + ' PRECEDING AND 1 FOLLOWING'
        aggregates = ([('MIN', 'v', 'lo'), ('MAX', 'v', 'hi')] if spec['minmax'] else [])
        if spec['collectors']:
            aggregates += [('COUNT', '*', 'c'), ('COUNT', 'v', 'cv'), ('SUM', 'v', 's')]
        fields = ['id'] + [g.alias(g.over(g.call(name, argument), 'p', 'id ASC', frame), label)
                           for name, argument, label in aggregates]
        result[case] = g.select(fields, g.derived(_source(), 's'), order='id')
    return result


def schema_and_seeds(n, case):
    cell(case, n)
    schema = 'CREATE TABLE t (id Int64 NOT NULL, PRIMARY KEY (id));'
    values = g.combine([g.select([g.alias(windows._int(i), 'id')]) for i in range(1, n + 1)])
    return schema, ['UPSERT INTO t ' + values]


def metadata(case, n):
    return {'generator_version': GENERATOR_VERSION, 'q01_resource_scaling_cell': cell(case, n),
            'table_service_overrides': {'EnableFallbackToYqlOptimizer': False},
            'pragmas': [('ydb.WindowFunctionsV2', 'true')], 'node_count': 1,
            'quality_requires_separate_plan_evidence': True, 'repetitions': 1,
            'peak_native_concurrency': 1, 'resource_scaling_verified': False}


def expected_rows(case, n):
    spec = cell(case, n)
    result = []
    for partition in range(3):
        ids = [i for i in range(1, n + 1) if i % 3 == partition]
        values = [None if i % 13 < 9 else i % 97 - 48 for i in ids]
        counts, sums = [0], [0]
        for value in values:
            counts.append(counts[-1] + (value is not None))
            sums.append(sums[-1] + (value or 0))
        minima, maxima, added = deque(), deque(), 0
        for position, identity in enumerate(ids):
            if spec['kind'] == 'rows':
                left, right = max(0, position - spec['frame_width']), min(len(ids), position + 2)
            else:
                left = bisect_left(ids, identity - spec['frame_width'])
                right = bisect_right(ids, identity + 1)
            while added < right:
                value = values[added]
                if value is not None:
                    while minima and values[minima[-1]] >= value:
                        minima.pop()
                    while maxima and values[maxima[-1]] <= value:
                        maxima.pop()
                    minima.append(added)
                    maxima.append(added)
                added += 1
            for queue in (minima, maxima):
                while queue and queue[0] < left:
                    queue.popleft()
            row = {'id': identity}
            if spec['minmax']:
                row.update(lo=values[minima[0]] if minima else None,
                           hi=values[maxima[0]] if maxima else None)
            if spec['collectors']:
                count = counts[right] - counts[left]
                row.update(c=right - left, cv=count, s=sums[right] - sums[left] if count else None)
            result.append(row)
    return sorted(result, key=lambda row: row['id'])


def check_rows(case, rows, n):
    encode = lambda values: [json.dumps(row, sort_keys=True, separators=(',', ':')) for row in values]
    return {'semantic_oracle_pass': encode(rows) == encode(expected_rows(case, n)),
            'resource_scaling_verified': False, 'quality_improvement_verified': False}


def check_physical_ast(case, text):
    spec = specifications()[case]
    summary = windows.physical_ast_summary(text)
    _, nodes = windows.parse_physical_graph(text)
    summary['range_minmax_calls'] = sum(bool(node) and node[0] == 'KqpRangeMinMax' for node in nodes)
    return {'physical_ast': summary, 'q01_scaling_family': spec['family'],
            'quality_oracle_pass': False, 'resource_scaling_verified': False,
            'quality_improvement_verified': False,
            'quality_pending_reason': 'raw_scaling_observation_has_no_admission_validator'}
