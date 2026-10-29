"""Offline identity bag/admission contracts; return counts, never workloads."""
from collections import Counter
import itertools

import c31_identity_checks as checks


def run_contracts():
    counts = Counter()
    for n in (8, 11, 17):
        queries = checks.recipes(n)
        for case in checks.CASES:
            shape, _, kind = checks._parts(case)
            schema, seeds = checks.schema_and_seeds(n, case)
            assert queries[case] and schema
            assert all(isinstance(seed, str) and seed for seed in seeds)
            assert checks.metadata(case, n)['correctness_requires_physical_ast']
            counts['compositional_native_profiles'] += 1
            expected = checks._expected(case, n)
            assert checks.check_rows(case, expected, n)
            counts['bag_oracle_acceptance_checks'] += 1
            if expected:
                assert not checks.check_rows(case, expected[:-1], n)
                assert not checks.check_rows(case, expected + expected[:1], n)
            else:
                assert not checks.check_rows(case, [{'extra': 1}], n)
            counts['bag_oracle_rejection_checks'] += 2 if expected else 1
            left, right = checks._logical_inputs(n, shape)
            actual = checks.reconstructed_model(left, right, kind,
                                                unique=shape.startswith('unique_'))
            assert checks._bag(actual) == checks._bag(expected)
            counts['domain_reconstruction_comparisons'] += 1
            if shape.startswith('domain_') and kind == 'left':
                naive = checks.reconstructed_model(left, right, kind, deduplicate=False)
                if checks._bag(naive) != checks._bag(expected):
                    counts['raw_rejoin_duplicate_bugs_detected'] += 1

    # Empty structs have one possible value but arbitrary bag multiplicity.
    # A constant grouping key gives one domain row iff input is nonempty.
    for multiplicity, right_count, kind in itertools.product(range(6), range(6), checks._KINDS):
        right = tuple(index for index in range(right_count) if index % 2 == 0)
        reference = Counter()
        for _ in range(multiplicity):
            if kind == 'left':
                reference.update(right or (None,))
            elif bool(right) == (kind == 'semi'):
                reference.update(('unit',))
        domain_rows = int(multiplicity > 0)
        inner = right * domain_rows
        reconstructed = Counter()
        for _ in range(multiplicity):
            if kind == 'left':
                reconstructed.update(inner or (None,))
            elif bool(inner) == (kind == 'semi'):
                reconstructed.update(('unit',))
        assert reconstructed == reference
        counts['zero_column_and_empty_bag_comparisons'] += 1

    for slot, optional, unique in itertools.product(checks._EXACT_SCALARS, (False, True), (False, True)):
        item = 'Optional<' + slot + '>' if optional else slot
        assert checks.identity_admission([item], unique)
        counts['admitted_type_contracts'] += 1
    excluded = ('Float', 'Double', 'TzDate', 'Pg', 'Struct', 'List', 'Dict',
                'Resource', 'Optional<Int64>', 'Null')
    for slot in excluded:
        item = 'Optional<' + slot + '>'
        assert not checks.identity_admission([item], False)
        assert not checks.identity_admission([item], True)
        counts['excluded_type_contracts'] += 2
    for predicate, subplans in itertools.product((False, True), repeat=2):
        assert checks.identity_admission(['Int64'], False, predicate, subplans) == (predicate and subplans)
        # Unique reconstruction changes no residual evaluation multiplicity.
        assert checks.identity_admission(['Optional<Int64>'], True, predicate, subplans)
        counts['repeatability_admission_contracts'] += 2
    assert checks.identity_admission([], False)
    assert counts['raw_rejoin_duplicate_bugs_detected'] > 0

    # These are parser-only synthetic graphs, never query plans or fixtures.
    good = '(return (DqPhyStage (GraceJoinCore) (StablePickle) (StablePickle) (DqPhyHashCombine)))'
    missing = '(return (DqPhyStage (GraceJoinCore)))'
    for shape in ('unique_mixed', 'domain_mixed'):
        case = 'c31_identity_' + shape + '__row__left'
        assert checks.check_physical_ast(case, good)['structural_oracle_pass']
        assert not checks.check_physical_ast(case, missing)['structural_oracle_pass']
        counts['synthetic_structural_oracle_checks'] += 2
    return {'status': 'offline_contracts_passed_native_pending', 'counts': dict(counts),
            'native_runs': 0, 'builds': 0, 'exact_reproductions_persisted': False,
            'excluded_cases_counted_as_supported': False}
