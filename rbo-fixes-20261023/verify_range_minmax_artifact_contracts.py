"""Check RANGE MIN/MAX generators and independent models, without native runs."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))
import quality_range_minmax_checks as checks


def shape(*, incremental=False, collector=True, fold=True):
    names = ['KqpRangeMinMax'] if incremental else []
    if collector:
        names += ['WinFramesCollector', 'WinFrame']
    if fold:
        names += ['Fold1']
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
        spec = checks._specifications()[case]
        accepted = checks.metadata(case, 61)['optimization_acceptance_case']
        control = spec.get('control')
        before = checks._summary(shape())
        after = shape(incremental=accepted, collector=not accepted or control in ('mixed', 'fallback', 'native'),
                      fold=not accepted or control == 'fallback')
        assert checks.check_physical_ast(case, after, before)['structural_oracle_pass']
        physical += 1
        if accepted:
            assert not checks.check_physical_ast(case, shape(), before)['structural_oracle_pass']
            physical += 1
    for malformed in ('diagnostic text', '(', '(KqpRangeMinMax)', '(DqPhyStage) trailing'):
        try:
            checks.check_physical_ast(next(iter(checks.recipes(8))), malformed)
        except ValueError:
            physical += 1
        else:
            raise AssertionError('Malformed physical input accepted')
    evidence = {'generator_cases': len(checks.recipes(61)), 'builder_oracle_contract_checks': builders,
                'physical_checker_contract_checks': physical, 'deque_model': checks.verify_deque_model(),
                'generator_sha256': hashlib.sha256(Path(checks.__file__).read_bytes()).hexdigest(),
                'native_runs': 0, 'compiled': False, 'production_edited': False,
                'generated_inputs_saved': False, 'sql_parser_verification_performed': False,
                'limitations': ['Native yield/cancel/memory-accounting tests are still required.',
                                'Physical-shape checks are synthetic contracts until real plans are captured.',
                                'Scalar Decimal special values have source proof but no generated SQL case.']}
    (ROOT / 'verification/q01-range-minmax-artifact-contracts.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, sort_keys=True))


if __name__ == '__main__':
    main()
