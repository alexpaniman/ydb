"""Source-only recipe checks; no engine, file inputs or process launches.

Generated queries, fixtures and oracle rows are kept only in memory. This
checks the declared shared-window contract, not an actual parser or lowering.
"""
import hashlib
import json
from pathlib import Path
import re
from unittest.mock import patch

import quality_q01_scaling_checks as scaling


def run_contract_checks():
    count = 0
    def check(value):
        nonlocal count
        if not value:
            raise AssertionError('scaling_recipe_metadata_contract_failed')
        count += 1

    primary = ['q01_scaling_' + family + '_w' + str(width)
               for family in scaling.FAMILIES for width in scaling.WIDTHS]
    check(len(primary) == 15)
    for n in scaling.ROWS:
        recipes = scaling.recipes(n)
        check(list(recipes) == primary + [scaling.SETUP_CASE])
        control_schema, control_seeds = scaling.schema_and_seeds(n, scaling.SETUP_CASE)
        control_meta = scaling.metadata(scaling.SETUP_CASE, n)
        check(control_meta['q01_observation_control'] is True)
        check(control_meta['q01_resource_scaling_cell']['control_kind'] == 'matched_setup_scan')
        check(control_meta['q01_resource_scaling_cell']['frame_width'] is None)
        check(control_meta['resource_scaling_verified'] is False)
        check(not re.search(r'\b(?:WINDOW|OVER)\b', recipes[scaling.SETUP_CASE]))
        check(recipes[scaling.SETUP_CASE].endswith(' ORDER BY id'))
        for case in primary:
            spec, query = scaling.cell(case, n), recipes[case]
            aggregates = (2 if spec['minmax'] else 0) + (3 if spec['collectors'] else 0)
            declarations = re.findall(r'\bWINDOW\s+([a-z_][a-z0-9_]*)\s+AS\s*\(', query)
            references = re.findall(r'\bOVER\s+([a-z_][a-z0-9_]*)', query)
            check(declarations == [scaling.WINDOW_NAME])
            check(references == [scaling.WINDOW_NAME] * aggregates)
            check(not re.search(r'\bOVER\s*\(', query))
            check(query.count('PARTITION BY p ORDER BY id ASC') == 1)
            check(query.count(' BETWEEN ') == 1)
            check(query.endswith(' ORDER BY id'))
            check(query.index(' WINDOW ') < query.rindex(' ORDER BY id'))
            schema, seeds = scaling.schema_and_seeds(n, case)
            check(schema == control_schema and seeds == control_seeds)
            meta = scaling.metadata(case, n)
            check(meta['q01_observation_control'] is False)
            for key in ('table_service_overrides', 'pragmas', 'node_count', 'repetitions',
                        'peak_native_concurrency', 'quality_requires_separate_plan_evidence'):
                check(meta[key] == control_meta[key])
            check(meta['resource_scaling_verified'] is False)

    # Required order and scalar types are part of the result contract. JSON
    # encoding deliberately distinguishes integer values from bool/float.
    n = scaling.ROWS[0]
    for case in primary + [scaling.SETUP_CASE]:
        rows = scaling.expected_rows(case, n)
        check(scaling.check_rows(case, rows, n)['semantic_oracle_pass'] is True)
        check(scaling.check_rows(case, list(reversed(rows)), n)['semantic_oracle_pass'] is False)
        check(scaling.check_rows(case, [rows[1], rows[0]] + rows[2:], n)['semantic_oracle_pass'] is False)
        for value in (True, float(rows[0]['id'])):
            changed = [dict(row) for row in rows]
            changed[0]['id'] = value
            check(scaling.check_rows(case, changed, n)['semantic_oracle_pass'] is False)
        for key in ('resource_scaling_verified', 'quality_improvement_verified'):
            check(scaling.check_rows(case, rows, n)[key] is False)
    for n in scaling.ROWS:
        rows = scaling.expected_rows(scaling.SETUP_CASE, n)
        check(len(rows) == n and all(row == {'id': i + 1} for i, row in enumerate(rows)))
        sample = scaling.validate_sample({'case': scaling.SETUP_CASE, 'rows': n,
            'run_id': 'a' * 32, 'kind': 'pilot', 'index': 0})
        check(sample['cell']['family'] == 'setup_scan')
        # The physical parser is tested elsewhere. Here substitute numeric
        # metadata only to assert that even the control cannot admit quality.
        with patch.object(scaling.windows, 'physical_ast_summary', return_value={}), \
             patch.object(scaling.windows, 'parse_physical_graph', return_value=(None, [])):
            physical = scaling.check_physical_ast(scaling.SETUP_CASE, '')
        for key in ('quality_oracle_pass', 'resource_scaling_verified', 'quality_improvement_verified'):
            check(physical[key] is False)

    return {'status': 'source_recipe_contracts_passed_native_unverified', 'assertions': count,
            'primary_profiles': 15, 'setup_control_profiles': 1, 'permitted_row_counts': len(scaling.ROWS),
            'generator_sha256': hashlib.sha256(Path(scaling.__file__).read_bytes()).hexdigest(),
            'native_runs': 0, 'real_process_launches': 0, 'rendered_inputs_or_results_persisted': False,
            'actual_frontend_and_physical_path_verified': False, 'resource_scaling_verified': False}


if __name__ == '__main__':
    print(json.dumps(run_contract_checks(), sort_keys=True))
