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
import quality_q01_path_checks as paths

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'q01_concrete_window_cells_v3'
WIDTHS = (16, 128, 512)
ROWS = (2048, 4096, 8192, 16384, 32768)
FAMILIES = ('rows_collectors', 'rows_minmax', 'range_minmax', 'range_mixed', 'range_collectors')
SETUP_CASE = 'q01_scaling_setup_scan'
WINDOW_NAME = 'q01_shared_window'


def specifications():
    result = {'q01_scaling_' + family + '_w' + str(width):
            {'family': family, 'frame_width': width, 'partition_count': 3,
             'kind': 'range' if family.startswith('range_') else 'rows',
             'minmax': family.endswith(('minmax', 'mixed')),
             'collectors': family.endswith(('collectors', 'mixed'))}
            for family in FAMILIES for width in WIDTHS}
    result[SETUP_CASE] = {'family': 'setup_scan', 'frame_width': None,
                         'partition_count': None, 'kind': None,
                         'minmax': False, 'collectors': False,
                         'control_kind': 'matched_setup_scan'}
    for family in ('rows_minmax', 'range_minmax', 'range_mixed'):
        result['q01_scaling_' + family + '_suffix'] = dict(
            family=family, frame_width=16, partition_count=3,
            kind='range' if family.startswith('range_') else 'rows',
            minmax=True, collectors=family == 'range_mixed',
            upper_unbounded=True, control_kind='general_suffix')
    for kind in ('rows', 'range'):
        result['q01_scaling_' + kind + '_fallback'] = dict(
            family=kind + '_fallback', frame_width=16, partition_count=3,
            kind=kind, minmax=True, collectors=True,
            fallback=True, control_kind='same_window_fallback')
    return result


def finite_profile_cases():
    primary = ['q01_scaling_' + family + '_w' + str(width)
               for family in ('rows_collectors', 'rows_minmax', 'range_minmax', 'range_mixed')
               for width in (16, 512)]
    return primary + ['q01_scaling_range_collectors_w512', SETUP_CASE] + [
        'q01_scaling_' + family + '_suffix'
        for family in ('rows_minmax', 'range_minmax', 'range_mixed')] + [
        'q01_scaling_rows_fallback', 'q01_scaling_range_fallback']


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
            for module in (g, windows, paths)}


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
        if case == SETUP_CASE:
            result[case] = g.select(['id'], g.derived(_source(), 's'), order='id')
            continue
        frame = spec['kind'].upper() + ' BETWEEN ' + str(spec['frame_width']) + ' PRECEDING AND 1 FOLLOWING'
        if spec.get('upper_unbounded'):
            frame = frame.rsplit(' AND ', 1)[0] + ' AND UNBOUNDED FOLLOWING'
        aggregates = ([('MIN', 'v', 'lo'), ('MAX', 'v', 'hi')] if spec['minmax'] else [])
        if spec['collectors']:
            aggregates += [('COUNT', '*', 'c'), ('COUNT', 'v', 'cv'), ('SUM', 'v', 's')]
        if spec.get('fallback'):
            aggregates += [('MIN', 'CAST(v AS Double)', 'fallback')]
        # Inline OVER clauses acquire distinct anonymous frontend names. One
        # explicit name puts every aggregate into the same logical window.
        fields = ['id'] + [g.alias(g.call(name, argument) + ' OVER ' + WINDOW_NAME, label)
                           for name, argument, label in aggregates]
        result[case] = (g.select(fields, g.derived(_source(), 's'))
                        + ' WINDOW ' + WINDOW_NAME + ' AS (PARTITION BY p ORDER BY id ASC '
                        + frame + ') ORDER BY id')
    return result


def schema_and_seeds(n, case):
    cell(case, n)
    schema = 'CREATE TABLE t (id Int64 NOT NULL, PRIMARY KEY (id));'
    values = g.combine([g.select([g.alias(windows._int(i), 'id')]) for i in range(1, n + 1)])
    return schema, ['UPSERT INTO t ' + values]


def metadata(case, n):
    return {'generator_version': GENERATOR_VERSION, 'q01_resource_scaling_cell': cell(case, n),
            'q01_observation_control': case == SETUP_CASE,
            'table_service_overrides': {'EnableFallbackToYqlOptimizer': False},
            'pragmas': [('ydb.WindowFunctionsV2', 'true')], 'node_count': 1,
            'quality_requires_separate_plan_evidence': True, 'repetitions': 1,
            'peak_native_concurrency': 1, 'resource_scaling_verified': False}


def expected_rows(case, n):
    spec = cell(case, n)
    if case == SETUP_CASE:
        return [{'id': i} for i in range(1, n + 1)]
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
            if spec.get('upper_unbounded'):
                right = len(ids)
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
            if spec.get('fallback'):
                # Values are bounded exact integers even though the aggregate
                # input/result is Double. RowsJson uses PREC_AUTO with no
                # trailing decimal padding, so its parsed JSON token is int.
                row['fallback'] = row['lo']
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
            'q01_path_witness': paths.observe(spec, text),
            'quality_pending_reason': 'concrete_acceptance_stage_A_requires_actual_path_and_resource_review'}
