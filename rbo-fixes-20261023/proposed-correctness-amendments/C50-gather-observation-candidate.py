"""Bounded C50 execution-plan observations, with no raw plan persistence.

An identified mixed-key fixture join must have two ordinary UnionAll inputs
and exactly one actual task. Public plan JSON does not expose original key
annotations or internal distribution metadata; neither property is claimed.
"""
import hashlib
import json
import re

import join_key_checks as original

CASE_SPECIFIC_SCHEMA = True
_CASES = {
    'c50_scope_direct_row': 'c50_int_double_inner_row',
    'c50_scope_grouped_se': 'c50_grouped_after_join_se',
    'c50_scope_chain_se': 'c50_join_chain_se',
}


def _base(case):
    if case not in _CASES:
        raise ValueError('Unknown C50 observation')
    return _CASES[case]


def recipes(n):
    source = original.recipes(n)
    return {case: source[base] for case, base in _CASES.items()}


def schema_and_seeds(n, case):
    return original.schema_and_seeds(n, _base(case))


def requirements(case, mode='new'):
    if mode != 'new':
        raise ValueError('C50 gather observations require the fixed new optimizer')
    if not re.fullmatch('[0-9a-f]{64}', getattr(original, '_verification_source_sha256', '') or ''):
        raise ValueError('C50 base generator requires an exact loaded source snapshot')
    result = dict(original.requirements(_base(case), mode))
    result.update(generator_version='c50_identified_gather_stage_v1',
                  correctness_requires_query_plan_evidence=True,
                  repetitions=1)
    return result


def check_rows(case, rows, n):
    return {'semantic_oracle_pass': original.semantic_oracle(_base(case), rows, n),
            'independent_multiset_oracle': True}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate query-plan key')
        result[key] = value
    return result


def _children(node):
    children = node.get('Plans', [])
    if not isinstance(children, list) or any(not isinstance(child, dict) for child in children):
        raise ValueError('Malformed query-plan children')
    return children


def _nodes(root):
    todo = [(root, ())]
    visited = 0
    while todo:
        node, parents = todo.pop()
        if not isinstance(node, dict):
            raise ValueError('Expected query-plan node')
        visited += 1
        if visited > 4096 or len(parents) > 128:
            raise ValueError('Query plan outside bounded observation')
        yield node, parents
        todo.extend((child, parents + (node,)) for child in _children(node))


def _operators(node):
    operators = node.get('Operators', [])
    if not isinstance(operators, list) or any(not isinstance(item, dict) for item in operators):
        raise ValueError('Malformed query-plan operator list')
    return operators


def _condition_matches(condition):
    if not isinstance(condition, str):
        return False
    match = re.fullmatch(r'\s*(a|jl|b|jr)\.k(?:_[0-9]+)?\s*=\s*'
                         r'(a|jl|b|jr)\.k(?:_[0-9]+)?\s*', condition)
    if not match:
        return False
    return ((match[1] in ('a', 'jl') and match[2] in ('b', 'jr'))
            or (match[2] in ('a', 'jl') and match[1] in ('b', 'jr')))


def _source_tables(connection):
    tables = set()
    for node, _ in _nodes(connection):
        if 'CTE Name' in node:
            raise ValueError('Unresolved shared producer in gather observation')
        for operator in _operators(node):
            if 'Table' in operator:
                table = operator['Table']
                if not isinstance(table, str):
                    raise ValueError('Malformed source table identifier')
                tables.add(table)
    return tables


def check_query_plan(case, text, n):
    _base(case)
    result = {'query_plan_sha256': hashlib.sha256(text.encode()).hexdigest(),
              'query_plan_parsed': False,
              'query_plan_structural_oracle_pass': False,
              'c50_gather_observer_version': 'identified_execution_join_stage_v1',
              'c50_observation_shape': {'c50_scope_direct_row': 'direct',
                                        'c50_scope_grouped_se': 'grouped',
                                        'c50_scope_chain_se': 'chain'}[case],
              'c50_intended_join_identified': False,
              'c50_two_nonparallel_union_inputs_observed': False,
              'c50_single_join_task_observed': False,
              'c50_source_type_mismatch_proved': False,
              'c50_internal_distribution_metadata_proved': False,
              'c50_shared_producer_backpressure_verified': False,
              'c50_base_generator_sha256': getattr(original, '_verification_source_sha256', None)}
    try:
        document = json.loads(text, object_pairs_hook=_unique_object)
        if not isinstance(document, dict) or not isinstance(document.get('Plan'), dict):
            raise ValueError('Expected one execution Plan document')
        nodes = list(_nodes(document['Plan']))
        stages, candidates = {}, []
        for node, parents in nodes:
            guid = node.get('StageGuid')
            if guid is None:
                continue
            if not isinstance(guid, str) or not guid or guid in stages:
                raise ValueError('Missing or repeated execution-stage identity')
            stages[guid] = node
            for operator in _operators(node):
                if _condition_matches(operator.get('Condition')):
                    candidates.append((node, operator, parents))
        result.update(query_plan_parsed=True, c50_execution_stage_count=len(stages),
                      c50_candidate_join_count=len(candidates))
        if len(candidates) != 1:
            return result
        stage, join, parents = candidates[0]
        inputs = _children(stage)
        if (join.get('JoinKind') != 'Inner' or join.get('JoinAlgo') not in ('Grace', 'BlockHash')
                or len(inputs) != 2
                or sum('JoinKind' in operator for operator in _operators(stage)) != 1):
            return result
        sources = [_source_tables(connection) for connection in inputs]
        if sorted(sorted(names) for names in sources) != [['jl'], ['jr']]:
            return result
        result['c50_intended_join_identified'] = True
        result['c50_two_nonparallel_union_inputs_observed'] = all(
            connection.get('PlanNodeType') == 'Connection'
            and connection.get('Node Type') == 'UnionAll'
            and 'Parallel' not in connection
            and len(_children(connection)) == 1 for connection in inputs)
        stats = stage.get('Stats')
        if isinstance(stats, dict):
            tasks = stats.get('Tasks')
            result['c50_single_join_task_observed'] = type(tasks) is int and tasks == 1
            if type(tasks) is int and tasks >= 0:
                result['c50_observed_join_tasks'] = tasks
        # These are observations along the chosen join's outgoing ancestor
        # path, not global callable counts or proofs of hidden RBO metadata.
        outgoing = [node for node in parents if node.get('PlanNodeType') == 'Connection']
        shuffles = [node for node in outgoing if node.get('Node Type') == 'HashShuffle']
        result['c50_downstream_shuffle_edges_observed'] = len(shuffles)
        result['c50_downstream_keyed_shuffle_observed'] = any(
            isinstance(node.get('KeyColumns'), list) and bool(node['KeyColumns'])
            and all(isinstance(key, str) and bool(key) for key in node['KeyColumns'])
            for node in shuffles)
        # A label for a composed query cannot be satisfied by an unrelated
        # single join. Require the expected consumer on this join's path.
        consumers = [operator for node in parents + (stage,) for operator in _operators(node)]
        aggregation = any(operator.get('Name') == 'Aggregate' and bool(operator.get('GroupBy'))
                          for operator in consumers)
        following_join = any(operator is not join and operator.get('JoinKind') == 'Inner'
                             for operator in consumers)
        shape = result['c50_observation_shape']
        result['c50_composition_observed'] = ((shape == 'direct' and not aggregation and not following_join)
                                             or (shape == 'grouped' and aggregation)
                                             or (shape == 'chain' and following_join))
        result['query_plan_structural_oracle_pass'] = all(result[key] for key in (
            'c50_intended_join_identified', 'c50_two_nonparallel_union_inputs_observed',
            'c50_single_join_task_observed', 'c50_composition_observed'))
    except (ValueError, TypeError, KeyError, RecursionError) as error:
        result['c50_plan_observation_rejection'] = type(error).__name__
    return result
