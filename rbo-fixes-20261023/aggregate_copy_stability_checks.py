"""Unassigned aggregate-copy candidate: anonymous builders and strict evidence gates.

No renderer output, seed rows, physical AST, or result values may be persisted.
This module launches no processes and does not assign a finding number. A plan
with copied reductions alone is not a confirmed semantic bug.
"""
from collections import Counter
import hashlib
import itertools
import json
from pathlib import Path
import re

import generate_and_verify as g
import quality_window_checks as windows
from optimizer_mode_contract import optimizer_mode_matches_record

CASE_SPECIFIC_SCHEMA = True
GENERATOR_VERSION = 'unassigned_aggregate_copy_consistency_v1'
MAIN = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
SOURCE_REVIEWED_REVISIONS = frozenset((MAIN, '24e1aae9a0677e8ed12f257a7944c50516e9cba4'))
MAX_ROWS = 96
_READS = frozenset(('KqpRowsSourceSettings', 'KqpWideReadTable', 'KqpWideReadTableRanges',
                   'KqpReadTable', 'KqpReadTableRanges', 'KqlReadTable'))
_REDUCTIONS = frozenset(('DqHashCombine', 'DqPhyHashCombine', 'DqHashFinalize',
                         'WideCombiner', 'WideCombinerWithSpilling', 'WideLastCombiner',
                         'CombineCore', 'Condense1'))
_FORBIDDEN = frozenset(('Random', 'RandomNumber', 'RandomUuid', 'Now', 'CurrentUtcDate',
                        'CurrentUtcDatetime', 'CurrentUtcTimestamp', 'Udf', 'ScriptUdf', 'SqlCall'))


def specs():
    return {'copy_stability_' + '_'.join((family, shape, 'shared' if shared else 'copied')):
            dict(family=family, shape=shape, shared=shared)
            for family, shape, shared in itertools.product(
                ('cancellation_interleaved', 'cancellation_banded', 'exact_double', 'exact_integer'),
                ('scalar', 'grouped'), (False, True))}


def _size(n):
    if type(n) is not int or not 12 <= n <= MAX_ROWS or n % 6:
        raise ValueError('Use a bounded multiple of six between twelve and ninety-six')


def _rows(n, spec):
    _size(n)
    groups = 3 if spec['shape'] == 'grouped' else 1
    per_group = n // groups
    result = []
    for group in range(groups):
        for position in range(per_group):
            if spec['family'].startswith('cancellation_'):
                # Generate balanced large terms plus exactly representable small
                # terms. Only reduction order is unstable; no volatile function
                # or mutable external input participates in the workload.
                balanced = per_group // 3 * 3
                role = position % 3 if spec['family'].endswith('interleaved') else position * 3 // balanced
                magnitude = 1 << (53 + group % 2)
                value = (magnitude if role == 0 else -magnitude if role == 2 else 1 + group) if position < balanced else 0
            else:
                value = (position * 7 + group) % 9 - 4
            result.append({'id': position * groups + group + 1, 'g': group, 'v': value})
    return sorted(result, key=lambda row: row['id'])


def schema_and_seeds(n, case):
    spec = specs()[case]
    rows = _rows(n, spec)
    value_type = 'Int64' if spec['family'] == 'exact_integer' else 'Double'
    # Three small shards permit distinct merge schedules without a large input
    # or additional server nodes. Values and insertion order remain fixed.
    cuts = (n // 3 + 1, 2 * n // 3 + 1)
    schema = 'CREATE TABLE copy_probe (id Int64 NOT NULL, g Int64 NOT NULL, v '
    schema += value_type + ' NOT NULL, PRIMARY KEY (id)) WITH (PARTITION_AT_KEYS = ('
    schema += ', '.join(map(str, cuts)) + '));'
    arms = [g.select([g.alias(g.typed(value_type if field == 'v' else 'Int64', row[field]), field)
                      for field in ('id', 'g', 'v')]) for row in rows]
    return schema, ['UPSERT INTO copy_probe ' + g.combine(arms)]


def recipes(n):
    _size(n)
    result = {}
    for case, spec in specs().items():
        grouped = spec['shape'] == 'grouped'
        fields = [g.alias(g.call('SUM', 'v'), 's'), g.alias(g.call('COUNT', '*'), 'n')]
        if grouped:
            fields.insert(0, 'g')
        producer = g.select(fields, 'copy_probe', group='g' if grouped else None)
        # There is one producer row per partition. MIN over that singleton must
        # equal the attached row exactly, independent of the legal SUM order.
        window = g.over(g.call('MIN', 's'), 'g' if grouped else None)
        fields = ['g' if grouped else g.alias(g.typed('Int64', 0), 'g'),
                  g.alias(g.call('CAST', 's AS Int64'), 'producer'),
                  g.alias(g.call('CAST', window + ' AS Int64'), 'window'), 'n']
        result[case] = g.select(fields, g.derived(producer, 'p'))
    return result


def metadata(case, n, mode='new'):
    _size(n)
    spec = specs()[case]
    if mode not in ('new', 'legacy'):
        raise ValueError('Unknown optimizer mode')
    return {'generator_version': GENERATOR_VERSION,
            'table_service_overrides': {'EnableNewRBO': mode == 'new', 'EnableFallbackToYqlOptimizer': False,
                'EnableQueryServiceSpilling': spec['shared'],
                'EnableSpillingInHashJoinShuffleConnections': spec['shared'],
                'EnableSpillingNodes': 'All' if spec['shared'] else 'None',
                'DqHashOperatorsUseBlocks': False, 'UseBlockHashJoin': False,
                'EnableInlineJoinFiltersAfterCBO': False, 'EnableNewRBOPhysicalStagePeephole': False},
            'pragmas': [('YqlSelect', 'force' if mode == 'new' else 'disable'),
                        ('ydb.WindowFunctionsV2', 'true'), ('kikimr.CostBasedOptimizationLevel', '0')],
            'node_count': 1, 'peak_native_concurrency': 1,
            'correctness_requires_physical_ast': True,
            'acceptance_scope': 'unassigned_candidate_actual_consistency_violation_only',
            'repetitions': 1, 'maximum_fixture_rows': MAX_ROWS,
            'no_volatile_expressions': True, 'no_residual_join_filter': True}


def check_rows(case, rows, n):
    spec = specs()[case]
    source = _rows(n, spec)
    expected = {key: [row['v'] for row in source if row['g'] == key] for key in sorted({row['g'] for row in source})}
    shape_ok = (len(rows) == len(expected) and all(isinstance(row, dict) and
        set(row) == {'g', 'producer', 'window', 'n'} and all(type(value) is int for value in row.values()) for row in rows))
    if shape_ok:
        shape_ok = (len({row['g'] for row in rows}) == len(expected) and
                    all(row['g'] in expected and row['n'] == len(expected[row['g']]) for row in rows))
    stable_ok = shape_ok and (spec['family'].startswith('cancellation_') or
                    all(row['producer'] == sum(expected[row['g']]) for row in rows))
    violations = sum(row['producer'] != row['window'] for row in rows) if shape_ok else 0
    return {'semantic_oracle_pass': bool(stable_ok and violations == 0),
            'copy_row_contract_pass': bool(stable_ok), 'copy_consistency_violations': violations,
            'copy_consistency_observed': bool(stable_ok and violations == 0),
            'candidate_runtime_violation_observed': bool(stable_ok and violations > 0),
            'candidate_confirmation_requires_plan_and_mode_pair': True}


def _head(node):
    return node[0] if isinstance(node, list) and node and isinstance(node[0], str) else None


def _walk(start, resolve, stop_stages=True):
    todo, seen = [start], set()
    while todo:
        node = resolve(todo.pop())
        if not isinstance(node, list) or id(node) in seen:
            continue
        seen.add(id(node))
        if stop_stages and _head(node) == 'DqPhyStage':
            continue
        yield node
        head = _head(node)
        todo.extend(node[1:] if head and not head.startswith(('$', "'", '"')) else node)


def _stage_dependencies(start, resolve):
    todo, seen, stages = [start], set(), set()
    while todo:
        node = resolve(todo.pop())
        if not isinstance(node, list) or id(node) in seen:
            continue
        seen.add(id(node))
        if _head(node) == 'DqPhyStage':
            stages.add(id(node))
            continue
        head = _head(node)
        todo.extend(node[1:] if head and not head.startswith(('$', "'", '"')) else node)
    return stages


def _atom(node, resolve):
    value = resolve(node)
    if not isinstance(value, str):
        return None
    value = value[1:] if value.startswith("'") else value
    if value.startswith('"'):
        value = json.loads(value)
    return value if isinstance(value, str) else None


def _probe_value_read(node, resolve):
    if len(node) < 4:
        return False
    table = resolve(node[1])
    if _head(table) != 'KqpTable' or len(table) != 5:
        return False
    path = _atom(table[1], resolve)
    columns = resolve(node[2 if _head(node) == 'KqpRowsSourceSettings' else 3])
    return (isinstance(path, str) and path.endswith('/copy_probe') and isinstance(columns, list)
            and any(_atom(column, resolve) == 'v' for column in columns))


def check_physical_ast(case, text, baseline=None):
    spec = specs()[case]
    resolve, nodes = windows.parse_physical_graph(text)
    stages = {id(node): node for node in nodes if _head(node) == 'DqPhyStage'}
    dependencies, reads, sums, calls, outputs = {}, {}, set(), {}, {}
    malformed = len(stages) > 128
    if malformed:
        raise ValueError('Probe exceeds the bounded stage graph')
    for identity, stage in stages.items():
        if len(stage) not in (4, 5):
            malformed = True
            continue
        dependencies[identity] = _stage_dependencies(stage[1], resolve)
        local = list(_walk(stage[1], resolve)) + list(_walk(stage[2], resolve))
        program_calls = Counter(_head(node) for node in _walk(stage[2], resolve))
        calls[identity] = program_calls
        read_nodes = [node for node in local if _head(node) in _READS]
        reads[identity] = bool(read_nodes) and all(_probe_value_read(node, resolve) for node in read_nodes)
        value_type = 'Int64' if spec['family'] == 'exact_integer' else 'Double'
        typed = any(_head(node) == 'DataType' and len(node) == 2 and _atom(node[1], resolve) == value_type
                    for node in _walk(stage[2], resolve))
        if typed and program_calls['AggrAdd'] and any(program_calls[name] for name in _REDUCTIONS):
            sums.add(identity)
    def ancestors(identity, active=frozenset()):
        if identity in active:
            raise ValueError('Cyclic stage dependencies')
        result = set(dependencies.get(identity, ()))
        for parent in tuple(result):
            result |= ancestors(parent, active | {identity})
        return result
    ancestry = {identity: ancestors(identity) for identity in stages}
    leaf_sums = {identity for identity in sums if not (ancestry[identity] & sums)}
    read_origins = {identity: {source for source in ancestry[identity] | {identity} if reads.get(source)}
                    for identity in leaf_sums}
    duplicated = any(left != right and read_origins[left] and read_origins[right] and
                     read_origins[left].isdisjoint(read_origins[right])
                     for left in leaf_sums for right in leaf_sums)
    for node in nodes:
        if _head(node) == 'DqOutput' and len(node) == 3:
            owner = resolve(node[1])
            port = resolve(node[2])
            if _head(owner) == 'DqPhyStage' and isinstance(port, str):
                outputs.setdefault(id(owner), set()).add(port)
    shared = any(len(ports) >= 2 and calls.get(owner, {}).get('Switch', 0) > 0 and
                 len((ancestry[owner] | {owner}) & leaf_sums) == 1
                 for owner, ports in outputs.items())
    forbidden = sum(_head(node) in _FORBIDDEN for node in nodes)
    return {'physical_ast_parsed': True, 'physical_ast_sha256': hashlib.sha256(text.encode()).hexdigest(),
            'copy_plan_summary_version': 1, 'copy_stage_count': len(stages),
            'copy_read_owner_count': sum(reads.values()), 'copy_leaf_sum_owner_count': len(leaf_sums),
            'copy_candidate_independent_reduction_topology': bool(duplicated and not malformed and not forbidden),
            'copy_candidate_shared_reduction_topology': bool(shared and len(leaf_sums) == 1 and not malformed and not forbidden),
            'copy_output_dataflow_confirmation_required': True,
            'copy_forbidden_callable_count': forbidden,
            'structural_assertions': {'recognized_stage_grammar': not malformed,
                                      'deterministic_expression_inventory': forbidden == 0},
            'candidate_confirmation_requires_runtime_violation': True}


def _binding_evidence_matches(proof, record, role):
    """A separate reviewer must inspect actual anonymous AST dataflow.

    Stage counts intentionally cannot populate this attestation. Bind the
    specific SUM state/update and both compared output values to execution
    owners before preparing it; preserve only this closed summary and hashes.
    """
    allowed = {'schema_version', 'method', 'role', 'revision', 'generator_sha256',
               'query_sha256', 'physical_ast_sha256', 'value_type', 'compared_output_lineages',
               'independent_evaluation_count', 'sum_update_type_bound', 'source_reads_bound',
               'comparison_outputs_bound', 'whole_partition_expansion_bound',
               'volatile_expression_count', 'c31_filter_reconstruction_present'}
    if not isinstance(proof, dict) or set(proof) != allowed:
        return False
    expected_evaluations = 2 if role == 'copied' else 1
    return (proof['schema_version'] == 'anonymous_aggregate_dataflow_review_v1'
            and proof['method'] == 'reviewed_actual_ast_dataflow' and proof['role'] == role
            and all(isinstance(proof[key], str) and re.fullmatch('[0-9a-f]{64}', proof[key])
                    and proof[key] == record.get(key)
                    for key in ('generator_sha256', 'query_sha256', 'physical_ast_sha256'))
            and proof['revision'] == record.get('revision') and proof['value_type'] == 'Double'
            and type(proof['compared_output_lineages']) is int and proof['compared_output_lineages'] == 2
            and type(proof['independent_evaluation_count']) is int
            and proof['independent_evaluation_count'] == expected_evaluations
            and all(proof[key] is True for key in ('sum_update_type_bound', 'source_reads_bound',
                    'comparison_outputs_bound', 'whole_partition_expansion_bound'))
            and type(proof['volatile_expression_count']) is int and proof['volatile_expression_count'] == 0
            and proof['c31_filter_reconstruction_present'] is False)


def assess_records(new, legacy, shared, copied_dataflow=None, shared_dataflow=None):
    """Return a closed verdict; copied plans or cross-run SUM differences alone fail admission."""
    current = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    records = (new, legacy, shared)
    common = all(isinstance(record, dict) and record.get('generator_module') == 'aggregate_copy_stability_checks'
                 and record.get('generator_sha256') == current
                 and record.get('generator_version') == GENERATOR_VERSION
                 and record.get('binary_provenance_verified') is True
                 and type(record.get('exit_code')) is int and record['exit_code'] == 0
                 and record.get('copy_row_contract_pass') is True
                 and record.get('physical_ast_parsed') is True
                 and record.get('structural_assertions', {}).get('recognized_stage_grammar') is True
                 and record.get('structural_assertions', {}).get('deterministic_expression_inventory') is True
                 and record.get('revision') in SOURCE_REVIEWED_REVISIONS
                 and optimizer_mode_matches_record(record, require_exclusive=True)
                 for record in records)
    if not common:
        return {'status': 'insufficient_bound_native_evidence', 'confirmed': False}
    case = new.get('case')
    if case not in specs() or specs()[case]['shared'] or not specs()[case]['family'].startswith('cancellation_'):
        return {'status': 'wrong_candidate_profile', 'confirmed': False}
    sharing_case = case.removesuffix('_copied') + '_shared'
    identity = ('schema_sha256', 'fixture_sha256', 'rows', 'revision')
    input_bound = (type(new.get('rows')) is int and 12 <= new['rows'] <= MAX_ROWS and new['rows'] % 6 == 0
                   and isinstance(new.get('schema_sha256'), str) and re.fullmatch('[0-9a-f]{64}', new['schema_sha256']) is not None
                   and isinstance(new.get('fixture_sha256'), list) and bool(new['fixture_sha256'])
                   and all(isinstance(item, str) and re.fullmatch('[0-9a-f]{64}', item) for item in new['fixture_sha256']))
    matched = (input_bound and new['mode'] == 'new' and legacy['mode'] == 'legacy' and shared['mode'] == 'new'
               and legacy.get('case') == case and shared.get('case') == sharing_case
               and all(new.get(key) == legacy.get(key) == shared.get(key) for key in identity)
               and isinstance(new.get('revision'), str) and re.fullmatch('[0-9a-f]{40}', new['revision']) is not None)
    witnessed = (matched and new.get('semantic_oracle_pass') is False
                 and new.get('candidate_runtime_violation_observed') is True
                 and type(new.get('copy_consistency_violations')) is int
                 and new['copy_consistency_violations'] > 0
                 and new.get('copy_candidate_independent_reduction_topology') is True
                 and _binding_evidence_matches(copied_dataflow, new, 'copied')
                 and legacy.get('semantic_oracle_pass') is True
                 and legacy.get('copy_consistency_observed') is True
                 and type(legacy.get('copy_consistency_violations')) is int and legacy['copy_consistency_violations'] == 0
                 and shared.get('semantic_oracle_pass') is True
                 and shared.get('copy_consistency_observed') is True
                 and type(shared.get('copy_consistency_violations')) is int and shared['copy_consistency_violations'] == 0
                 and shared.get('copy_candidate_shared_reduction_topology') is True
                 and _binding_evidence_matches(shared_dataflow, shared, 'shared'))
    return {'status': 'runtime_invariant_violation_with_copied_producer' if witnessed else 'candidate_not_confirmed',
            'confirmed': bool(witnessed), 'same_statement_consistency_required': True,
            'candidate_copy_topology_observed': bool(new.get('copy_candidate_independent_reduction_topology')),
            'actual_output_dataflow_reviewed': bool(_binding_evidence_matches(copied_dataflow, new, 'copied')
                                                   and _binding_evidence_matches(shared_dataflow, shared, 'shared')),
            'requested_modes_bound': bool(matched), 'new_finding_number_assigned': False}
