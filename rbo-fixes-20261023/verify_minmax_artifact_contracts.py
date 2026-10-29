"""Verify generated MIN/MAX acceptance contracts without native execution."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))
import quality_minmax_checks as checks


def shape(*, stack=False, general=False, incremental_range=False):
    if incremental_range:
        return '(DqPhyStage ((KqpRangeMinMax)))'
    names = ['WinFramesCollector', 'WinFrame', 'WinFrame']
    if stack:
        names += ['Fold', 'Prepend', 'Skip', 'TakeWhile', 'Chain1Map']
    if general:
        names.append('Fold1')
    return '(DqPhyStage (' + ' '.join('(' + name + ')' for name in names) + '))'


def main():
    builders = physical = 0
    for n in (8, 24, 61):
        for case, query in checks.recipes(n).items():
            schema, seeds = checks.schema_and_seeds(n, case)
            assert query and schema and seeds
            assert checks.metadata(case, n)['peak_native_concurrency'] == 1
            expected = checks.expected_rows(case, n)
            assert checks.check_rows(case, expected, n)['semantic_oracle_pass']
            wrong = list(expected)
            if wrong:
                wrong[0] = dict(wrong[0], id=-17)
            else:
                wrong = [{'id': -17}]
            assert not checks.check_rows(case, wrong, n)['semantic_oracle_pass']
            builders += 4
    for case in checks.recipes(61):
        admission = checks.metadata(case, 61)['optimization_acceptance_case']
        control = checks._specifications()[case].get('control')
        fallback = control == 'fallback'
        baseline = checks._summary(shape(general=True))
        actual = shape(stack=admission, general=fallback or not admission, incremental_range=control == 'range')
        assert checks.check_physical_ast(case, actual, baseline)['structural_oracle_pass']
        physical += 1
        if admission:
            assert not checks.check_physical_ast(case, shape(general=True), baseline)['structural_oracle_pass']
            physical += 1
    for malformed in ('diagnostic text', '(', '(Fold1)', '(DqPhyStage) trailing'):
        try:
            checks.check_physical_ast(next(iter(checks.recipes(8))), malformed)
        except ValueError:
            physical += 1
        else:
            raise AssertionError('Malformed physical input accepted')
    model = checks.verify_stack_model()
    assert model['model_pass']
    evidence = {'generator_cases': len(checks.recipes(61)), 'builder_oracle_contract_checks': builders,
                'physical_checker_contract_checks': physical, 'stack_model': model,
                'generator_sha256': hashlib.sha256(Path(checks.__file__).read_bytes()).hexdigest(),
                'native_runs': 0, 'compiled': False, 'production_edited': False,
                'generated_inputs_saved': False, 'sql_parser_verification_performed': False}
    (ROOT / 'verification' / 'q01-minmax-expanded-artifact-contracts.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, sort_keys=True))


if __name__ == '__main__':
    main()
