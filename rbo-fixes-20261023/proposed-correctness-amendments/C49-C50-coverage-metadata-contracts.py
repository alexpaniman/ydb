"""Pure metadata/oracle checks for dormant coverage; no native workload runs."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import types


def run():
    root = Path(__file__).resolve().parent
    count = 0
    def check(condition):
        nonlocal count
        if not condition:
            raise AssertionError('Coverage metadata contract failed')
        count += 1

    saved = {name: sys.modules.get(name) for name in (
        'generate_and_verify', 'partial_lookup_key_generator', 'join_key_checks')}
    try:
        for name in saved:
            sys.modules[name] = types.ModuleType(name)
        sys.modules['join_key_checks']._verification_source_sha256 = 'a' * 64
        def load(name, filename):
            module = types.ModuleType(name)
            source = (root / filename).read_bytes()
            exec(compile(source, filename, 'exec'), module.__dict__)
            return module
        c49 = load('c49_candidate', 'C49-identity-controls-candidate.py')
        c50 = load('c50_candidate', 'C50-gather-observation-candidate.py')

        for parts in (2, 3):
            rows, _ = c49._rows(8, parts)
            selected = [row for row in rows if row[-2] == 1]
            check(len({row[:parts] for row in selected}) == len(selected))
            check(len({row[:1] for row in selected}) < len(selected))
            if parts == 3:
                check(len({row[:2] for row in selected}) < len(selected))
        for case in c49._SPECS:
            expected = c49.expected_rows(case, 8)
            check(c49.check_rows(case, expected, 8)['semantic_oracle_pass'] is True)
            check(c49.check_rows(case, expected + expected[:1], 8)['semantic_oracle_pass'] is False)
            check(c49.check_rows(case, expected[1:], 8)['semantic_oracle_pass'] is False)
            if 'complete' in case:
                dropped = [{key: value for key, value in row.items() if not key.startswith('preserved_')}
                           for row in expected]
                check(c49.check_rows(case, dropped, 8)['semantic_oracle_pass'] is False)

        def source(table):
            return {'StageGuid': 'source_' + table, 'Operators': [{'Table': table}],
                    'Stats': {'Tasks': 4}}
        def connection(table):
            return {'PlanNodeType': 'Connection', 'Node Type': 'UnionAll', 'Plans': [source(table)]}
        def base():
            return {'Plan': {'StageGuid': 'join_stage',
                    'Operators': [{'JoinKind': 'Inner', 'JoinAlgo': 'Grace', 'Condition': 'a.k = b.k'}],
                    'Stats': {'Tasks': 1}, 'Plans': [connection('jl'), connection('jr')]}}
        def verdict(document, case='c50_scope_direct_row'):
            return c50.check_query_plan(case, json.dumps(document), 8)
        good = verdict(base())
        check(good['query_plan_structural_oracle_pass'] is True)
        for key in ('c50_source_type_mismatch_proved', 'c50_internal_distribution_metadata_proved',
                    'c50_shared_producer_backpressure_verified'):
            check(good[key] is False)
        for tasks in (None, True, 1.0, 0, 2, '1', -1):
            document = base(); document['Plan']['Stats']['Tasks'] = tasks
            check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for edge in ('HashShuffle', 'Map', 'Broadcast', 'ParallelUnionAll'):
            document = base(); document['Plan']['Plans'][0]['Node Type'] = edge
            check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for table in ('wrong', 'jr'):
            document = base(); document['Plan']['Plans'][0]['Plans'][0]['Operators'][0]['Table'] = table
            check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for condition in ('a.id = b.id', 'a.k = a.k', 'a.k = b.k, a.id = b.id', None):
            document = base(); document['Plan']['Operators'][0]['Condition'] = condition
            check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for item in (False, True, 'False', 'True'):
            document = base(); document['Plan']['Plans'][0]['Parallel'] = item
            check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for kind in ('Left', 'Cross', None):
            document = base(); document['Plan']['Operators'][0]['JoinKind'] = kind
            check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        document = base(); document['Plan']['Plans'][0]['CTE Name'] = 'unresolved'
        check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        document = base(); document['Plan']['Plans'][1]['Plans'][0]['StageGuid'] = 'source_jl'
        check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        document = base(); document['Plan']['Operators'].append(deepcopy(document['Plan']['Operators'][0]))
        check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        document = base(); document['Plan']['Operators'].append({'JoinKind': 'Inner'})
        check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for case in ('c50_scope_grouped_se', 'c50_scope_chain_se'):
            check(verdict(base(), case)['query_plan_structural_oracle_pass'] is False)
        document = base()
        child = document['Plan']
        document['Plan'] = {'StageGuid': 'aggregate_stage',
                            'Operators': [{'Name': 'Aggregate', 'GroupBy': 'a.id'}],
                            'Plans': [{'PlanNodeType': 'Connection', 'Node Type': 'HashShuffle',
                                       'KeyColumns': ['a.id'], 'Plans': [child]}]}
        grouped = verdict(document, 'c50_scope_grouped_se')
        check(grouped['query_plan_structural_oracle_pass'] is True)
        check(grouped['c50_downstream_keyed_shuffle_observed'] is True)
        check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        document = base(); child = document['Plan']
        document['Plan'] = {'StageGuid': 'next_join',
                            'Operators': [{'JoinKind': 'Inner', 'Condition': 'a.id = c.id'}],
                            'Plans': [child, source('jt')]}
        check(verdict(document, 'c50_scope_chain_se')['query_plan_structural_oracle_pass'] is True)
        check(verdict(document)['query_plan_structural_oracle_pass'] is False)
        for text in ('{}', '[]', '{"Plan": {}, "Plan": {}}', 'not JSON'):
            check(c50.check_query_plan('c50_scope_direct_row', text, 8)['query_plan_structural_oracle_pass'] is False)
        return {'assertions_passed': count, 'metadata_and_oracle_only': True,
                'native_runs': 0, 'production_queries_rendered': False,
                'production_plans_used': False, 'raw_metadata_persisted': False}
    finally:
        for name, module in saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module


if __name__ == '__main__':
    print(json.dumps(run(), sort_keys=True))
