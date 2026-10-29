"""Check artifact contracts in memory; save aggregate verdicts, never inputs."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))
import quality_sum_checks as sums
import quality_partition_checks as parts


def abstract_plan(shuffles, *, fold=False, window=True, filtering=False):
    nodes = ['(DqCnHashShuffle)'] * shuffles
    if fold:
        nodes += ['(Fold1)']
    if window:
        nodes += ['(WinFramesCollector)', '(WinFrame)', '(WinFrame)']
    if filtering:
        nodes += ['(WideFilter)']
    return '(DqPhyStage (' + ' '.join(nodes) + '))'


def main():
    checks = {}
    for module in (sums, parts):
        count = 0
        for n in (8, 24, 61):
            for case, query in module.recipes(n).items():
                schema, seeds = module.schema_and_seeds(n, case)
                assert isinstance(query, str) and query and schema and seeds
                assert module.metadata(case, n)['peak_native_concurrency'] == 1
                expected = module.expected_rows(case, n)
                assert module.check_rows(case, expected, n)['semantic_oracle_pass']
                bad = list(expected)
                if bad:
                    bad[0] = dict(bad[0], id=-12345)
                else:
                    bad = [{'id': -12345}]
                assert not module.check_rows(case, bad, n)['semantic_oracle_pass']
                count += 4
        checks[module.__name__] = {
            'generator_sha256': hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest(),
            'cases': len(module.recipes(61)), 'generator_contract_checks': count}
    algebra = sums.verify_prefix_algebra()
    assert algebra['algebra_oracle_pass']
    checks['quality_sum_checks']['algebra'] = algebra
    shape_checks = 0
    for case in parts.recipes(61):
        positive = case in ('q15_same_partition', 'q15_intervening_filter', 'q15_override_enabled',
                            'q15_alias_filter', 'q15_payload_map')
        expected = 0 if case == 'q15_keyless_control' else 1 if positive else 2
        filtered = case in ('q15_intervening_filter', 'q15_alias_filter', 'q15_payload_map',
                            'q15_computed_key_control', 'q15_ordered_keys_control', 'q15_subset_keys_control')
        before = parts._summary(abstract_plan(2 if positive else expected, filtering=filtered))
        assert parts.check_physical_ast(case, abstract_plan(expected, filtering=filtered), before)['structural_oracle_pass']
        assert not parts.check_physical_ast(case, abstract_plan(expected + 1, filtering=filtered), before)['structural_oracle_pass']
        shape_checks += 2
    for case in sums.recipes(61):
        control = case.endswith('_control') and case != 'q01_sum_native_control'
        after = abstract_plan(0, fold=control, window=not control)
        before = sums.windows.physical_ast_summary(abstract_plan(0, fold=True, window=not control))
        assert sums.check_physical_ast(case, after, before)['structural_oracle_pass']
        shape_checks += 1
    checks['shape_contract_checks'] = shape_checks
    checks.update(native_runs=0, production_edited=False, generated_inputs_saved=False)
    (ROOT / 'verification' / 'quality-amendment-generator-contract.json').write_text(json.dumps(checks, indent=2) + '\n')
    legacy = dict(checks['quality_sum_checks'], native_runs=0, production_edited=False, generated_inputs_saved=False)
    (ROOT / 'verification' / 'q01-sum-amendment-model-check.json').write_text(json.dumps(legacy, indent=2) + '\n')
    print(json.dumps(checks, sort_keys=True))


if __name__ == '__main__':
    main()
