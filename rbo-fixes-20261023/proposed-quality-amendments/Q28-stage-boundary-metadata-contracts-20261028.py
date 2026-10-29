"""Bounded preparation checks; only generators and sanitized totals persist."""
import ast
from collections import Counter
import copy
import hashlib
import importlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    sys.dont_write_bytecode = True
    root = Path(__file__).resolve().parent.parent
    plan = json.loads((root / 'verification/idle-artifact-application-plan-20261028-complete.json').read_bytes())
    patch = root / 'proposed-quality-amendments/Q28-stage-boundary-semantic-profiles-20261028.patch'
    count = 0

    def check(value):
        nonlocal count
        if not value:
            raise AssertionError('Q28 metadata preparation check failed')
        count += 1

    with tempfile.TemporaryDirectory(prefix='q28-semantic-metadata-') as folder:
        base = Path(folder); here = base / root.name; here.mkdir()
        for path in root.glob('*.py'):
            shutil.copyfile(path, here / path.name)
        for name in plan['files']:
            if (root / name).is_file():
                shutil.copyfile(root / name, here / name)
        for entry in plan['patches'] + [{'patch': str(patch), 'sha256': hashlib.sha256(patch.read_bytes()).hexdigest()}]:
            path = root / entry['patch']
            check(hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'])
            process = subprocess.run(['git', 'apply', str(path)], cwd=base, capture_output=True)
            check(process.returncode == 0)
        for name in ('quality_pairing.py', 'quality_scheduler_semantic_checks.py'):
            ast.parse((here / name).read_bytes()); check(True)
        sys.path[:0] = [str(here), str(root), str(root.parent / 'rbo-review-20261021')]
        module = importlib.import_module('quality_scheduler_semantic_checks')
        pairing = importlib.import_module('quality_pairing')
        check(pairing.generator_modules('Q28') == ['quality_scheduler_semantic_checks'])
        owners = pairing.selected_cases('Q28', pairing.generator_modules('Q28'), 24)
        check(owners == dict.fromkeys(module.CASES, 'quality_scheduler_semantic_checks'))
        records = []
        for size in (8, 12, 24, 32):
            queries = module.recipes(size)
            check(tuple(queries) == module.CASES and len(queries) == 8)
            check(queries['q28_shared_spilling_control'] == queries['q28_shared_copied_control'])
            for case in module.CASES:
                schema, seeds = module.schema_and_seeds(size, case)
                check(bool(schema) and len(seeds) == len(module._tables(case)))
                expected = module.expected_rows(case, size)
                check(bool(expected) and len(expected) < 10000)
                check(module.check_rows(case, expected, size)['semantic_oracle_pass'])
                check(module.check_rows(case, list(reversed(expected)), size)['semantic_oracle_pass'])
                check(not module.check_rows(case, expected[:-1], size)['semantic_oracle_pass'])
                check(not module.check_rows(case, expected + [expected[0]], size)['semantic_oracle_pass'])
                changed = copy.deepcopy(expected)
                field = next(iter(changed[0])); changed[0][field] = 999999
                check(not module.check_rows(case, changed, size)['semantic_oracle_pass'])
                changed = copy.deepcopy(expected); changed[0]['unexpected'] = 1
                check(not module.check_rows(case, changed, size)['semantic_oracle_pass'])
                changed = copy.deepcopy(expected)
                field = next(key for key, value in changed[0].items() if type(value) is int)
                changed[0][field] = True
                check(not module.check_rows(case, changed, size)['semantic_oracle_pass'])
                meta = module.metadata(case, size)
                check(meta['node_count'] == meta['peak_native_concurrency'] == 1)
                check(meta['quality_requires_query_plan_evidence'] is True and meta['optimizer_stage_timing_available'] is False)
                check(meta['table_service_overrides']['EnableFallbackToYqlOptimizer'] is False)
                verdict = module.check_rows(case, expected, size)
                check(all(verdict[field] is False for field in ('query_rule_reachability_verified', 'direct_ir_contracts_verified', 'quality_acceptance_pass', 'quality_improvement_verified')))
                if case not in ('q28_map_filter_sort_limit_control', 'q28_conditional_guard_control'):
                    check(verdict['q28_expected_duplicate_groups'] > 0)
                    check(any(row.get('left_key') is None for row in expected))
                if case == 'q28_full_outer_nullable_control':
                    check(any(row['left_key'] is not None and row['right_key'] is None for row in expected))
                records.append({'case': case, 'rows_per_table': size, 'expected_row_count': len(expected),
                                'duplicate_groups': verdict['q28_expected_duplicate_groups'],
                                'query_sha256': hashlib.sha256(queries[case].encode()).hexdigest()})
                del schema, seeds, expected, changed
            del queries
        # Independent count oracle for the nullable RIGHT join, preserving every
        # occurrence and treating optional equality separately from Python None.
        for size in (8, 12, 24, 32):
            left = module._source(size, 'left'); right = module._source(size, 'right')
            occurrences = Counter(row['k'] for row in left if row['k'] is not None)
            wanted = sum(max(occurrences[row['k']], 1) if row['k'] is not None else 1 for row in right)
            check(len(module.expected_rows('q28_right_nullable_bag', size)) == wanted)
            check(module.expected_rows('q28_shared_spilling_control', size) == module.expected_rows('q28_shared_copied_control', size))
        for value in (True, 0, 7, 33, 1000, '24', None):
            try:
                module.recipes(value)
            except ValueError:
                check(True)
            else:
                check(False)
        for case in module.CASES:
            sources = [{'Node Type': 'TableFullScan', 'Operators': [{'Table': table}]} for table, _ in module._tables(case)]
            join_count = 2 if case == 'q28_nested_right_bag' else int(len(sources) > 1)
            joins = [{'Node Type': 'Join', 'Operators': [{'JoinKind': 'Full' if case == 'q28_full_outer_nullable_control' else 'Left'}]} for _ in range(join_count)]
            document = {'SimplifiedPlan': {'Node Type': 'ResultSet',
                            'OptimizerStats': {'CBOTreesTotal': 2, 'CBOTreesOptimized': 0}, 'Plans': sources + joins},
                        'Plan': {'Node Type': 'Query', 'Plans': [{'StageGuid': 'metadata-stage', 'Operators': []}]}}
            def observe(value):
                return module.check_query_plan(case, json.dumps(value), 24)
            observed = observe(document)
            check(observed['query_plan_parsed'] and observed['q28_final_plan_profile_complete'])
            check(observed['q28_cbo_trees_total'] == 2 and observed['q28_cbo_trees_optimized'] == 0)
            check(observed['q28_execution_stage_definitions'] == 1)
            check(all(observed[field] is False for field in ('optimizer_compilation_timing_available',
                'optimizer_stage_timing_available', 'logical_stage_boundary_execution_observed',
                'query_rule_reachability_verified', 'shared_producer_identity_verified',
                'direct_scheduler_work_counts_verified', 'quality_acceptance_pass', 'quality_improvement_verified')))
            check(not pairing._fixed_acceptance(dict(observed, case=case), dict(observed, case=case)))
            for key, value in (('CBOTreesTotal', True), ('CBOTreesTotal', -1), ('CBOTreesOptimized', 3)):
                invalid = copy.deepcopy(document); invalid['SimplifiedPlan']['OptimizerStats'][key] = value
                check(observe(invalid)['q28_final_plan_profile_complete'] is False)
            for key in ('SimplifiedPlan', 'Plan'):
                invalid = copy.deepcopy(document); del invalid[key]
                check(observe(invalid)['query_plan_parsed'] is False)
            invalid = copy.deepcopy(document); invalid['SimplifiedPlan']['Plans'][0]['Operators'][0]['Table'] = 'unrelated'
            check(observe(invalid)['q28_final_plan_profile_complete'] is False)
            invalid = copy.deepcopy(document); invalid['SimplifiedPlan']['Plans'] = ['malformed']
            check(observe(invalid)['q28_final_plan_profile_complete'] is False)
            invalid = copy.deepcopy(document); invalid['Plan']['Plans'].append({'StageGuid': 'metadata-stage'})
            check(observe(invalid)['q28_final_plan_profile_complete'] is False)
            invalid = copy.deepcopy(document); invalid['Plan']['Plans'].append({'CTE Name': 'unbound'})
            check(observe(invalid)['q28_final_plan_profile_complete'] is False)
            shared = copy.deepcopy(document)
            shared['Plan']['Plans'][0]['Subplan Name'] = 'CTE metadata-shared'
            shared['Plan']['Plans'].append({'CTE Name': 'metadata-shared'})
            check(observe(shared)['q28_shared_stage_reference_occurrences'] == 1)
            shared['Plan']['Plans'][0]['Stats'] = {'CpuTimeUs': 987654321}
            shared['Compilation'] = {'CpuTimeUs': 1234567, 'DurationUs': 7654321}
            check(observe(shared)['optimizer_compilation_timing_available'] is False)
            check(observe(shared)['optimizer_stage_timing_available'] is False)
            check('987654321' not in json.dumps(observe(shared)))
            check(module.check_query_plan(case, '{"Plan":{},"Plan":{}}', 24)['query_plan_parsed'] is False)
        del document, sources, joins
    return {'schema_version': 'q28_stage_boundary_metadata_preparation_v1', 'assertions_passed': count,
            'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(), 'profile_count': 8,
            'row_sizes_checked': [8, 12, 24, 32], 'cases': records,
            'temporary_26_stack_plus_candidate_application': True, 'queries_rendered_only_in_ram': True,
            'actual_query_results_used': False, 'actual_plans_used': False, 'native_runs': 0, 'builds': 0,
            'graphs': 0, 'toolchain_runs': 0, 'generated_queries_or_plans_persisted': False,
            'production_or_live_helper_edits': False}


if __name__ == '__main__':
    print(json.dumps(main(), sort_keys=True))
