"""Offline contracts for compositional C31 coverage; no native execution."""
import hashlib
import json
from pathlib import Path

import c31_identity_checks as checks
from verification_contract import record_verdict


def _shape(*, switch=True, joins=2, aggregate=True):
    """Build anonymous parser-shape controls, not a runnable query plan."""
    nodes = []
    if aggregate:
        nodes.append('(DqPhyHashCombine)')
    nodes.extend('(GraceJoinCore)' for _ in range(joins))
    if switch:
        nodes.append('(Switch $input 128 (0) (lambda (x) x) (0) (lambda (y) y))')
    return '(DqPhyStage (' + ' '.join(nodes) + '))'


def run():
    counts = dict(case_profiles=len(checks.CASES), added_profiles=len(checks._STABILITY_CASES),
                  guard_profiles=0, shared_profiles=0, rows_accepted=0, row_mutations_rejected=0,
                  recipes_built_in_memory=0, schemas_built_in_memory=0,
                  exact_float_order_models=0, structural_parser_contracts=0,
                  strict_guard_verdict_contracts=0)
    for size in (8, 24, 53):
        recipes = checks.recipes(size)
        assert len(recipes) == len(checks.CASES)
        counts['recipes_built_in_memory'] += len(recipes)
        for case in checks.CASES:
            schema, seeds = checks.schema_and_seeds(size, case)
            assert schema and seeds and recipes[case]
            counts['schemas_built_in_memory'] += 1
            rows = checks._expected(case, size)
            assert checks.check_rows(case, rows, size)
            counts['rows_accepted'] += 1
            changed = rows[:-1] if rows else [{'unexpected': 1}]
            assert not checks.check_rows(case, changed, size)
            counts['row_mutations_rejected'] += 1
            meta = checks.metadata(case, size)
            if size == 8:
                counts['guard_profiles'] += bool(meta.get('expected_rejection_token'))
                counts['shared_profiles'] += bool(meta.get('expected_shared_producer'))
            if meta.get('expected_rejection_token'):
                assert not meta['correctness_requires_physical_ast']
                assert meta['expected_rejection_token'] == checks._STABILITY_GUARD
                assert not meta['expected_identity_admission']
        left, _ = checks._stability_tables(size)
        for key in {row['k'] for row in left}:
            values = [row['v'] for row in left if row['k'] == key]
            for offset in range(len(values)):
                permutation = values[offset:] + values[:offset]
                for ordered in (permutation, list(reversed(permutation))):
                    assert sum(map(float, ordered)) == sum(values)
                    counts['exact_float_order_models'] += 1

    for case in checks._STABILITY_CASES:
        _, mode, _ = checks._parts(case)
        if mode != 'spilling':
            continue
        assert checks.check_physical_ast(case, _shape())['structural_oracle_pass']
        for variant in (_shape(switch=False), _shape(joins=1), _shape(aggregate=False)):
            assert not checks.check_physical_ast(case, variant)['structural_oracle_pass']
        counts['structural_parser_contracts'] += 4

    record = dict(expected_rejection_configured=True, correctness_guard_observed=True,
                  exit_code=1, failure_stage='target', correctness_physical_ast_required=False)
    assert record_verdict(record) == 'guard'
    counts['strict_guard_verdict_contracts'] += 1
    for changes in (dict(correctness_guard_observed=False), dict(correctness_guard_observed=None),
                    dict(exit_code=0), dict(exit_code=-6), dict(failure_stage='setup'),
                    dict(timeout=True), dict(harness_exception=True),
                    dict(native_batch_guard_pass=False), dict(correctness_physical_ast_required=True)):
        assert record_verdict(dict(record, **changes)) == 'failure'
        counts['strict_guard_verdict_contracts'] += 1
    for success in (True, False):
        assert record_verdict(dict(record, correctness_guard_observed=False,
                                   semantic_oracle_pass=success, exit_code=0)) == 'failure'
        counts['strict_guard_verdict_contracts'] += 1
    return counts


def main():
    root = Path(__file__).resolve().parent
    report = {
        'finding': 'C31', 'status': 'offline_generator_contracts_pass_native_pending',
        'production_revision': '24e1aae9a0677e8ed12f257a7944c50516e9cba4',
        'patch_sha256': 'dd0d5369f0f0ae42655582be6e79424276cac7cea4d4da9e6bbbd7eb5a1d2801',
        'generator_sha256': hashlib.sha256(Path(checks.__file__).read_bytes()).hexdigest(),
        'contract_generator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'contracts': run(),
        'native_guard': 'Only the exact copied-values guard token at the target request counts as a guard. Unrelated failures and unexpected successful admission fail.',
        'semantic_controls': 'Float SUM inputs are small exact integers; independent bag results are invariant under all sampled reduction orders. Int64 SUM controls must remain admitted.',
        'physical_controls': 'Spill controls require aggregation, reconstruction joins and a reachable multi-output Switch. Block controls require residual-supporting block joins.',
        'remaining_native_acceptance': [
            'Actual planner reachability, result bags and exact guard admission remain pending guarded native execution.',
            'Keyed producer has one intended reconstruction hub; domain UNION ALL can introduce additional sharing, so the exact Switch origin still needs sanitized native AST review.',
            'These tests do not verify broader floating/composite identity support or volatile occurrence semantics.',
            'The 24-case direct-IR adapter has separate contracts and does not yet test aggregate copy stability.'
        ],
        'native_executed': False, 'compiled': False, 'production_sources_edited': False,
        'generated_reproductions_persisted': False,
    }
    target = root / 'verification/C31-copy-stability-generator-contracts.json'
    target.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'contracts': report['contracts'],
                      'generator_sha256': report['generator_sha256'],
                      'report_sha256': hashlib.sha256(target.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
