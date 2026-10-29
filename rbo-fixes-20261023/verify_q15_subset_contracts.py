"""Run low-memory Q15 artifact/model checks; retain scalar evidence only."""
import hashlib
import json
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))
import quality_partition_subset_checks as native
import quality_partition_cpp_checks as cpp
import quality_window_checks as windows
from verify_quality_amendment_models import abstract_plan


def select_key(upstream, translated):
    selected = []
    for key in upstream:
        match = next((index for index, candidate in enumerate(translated) if candidate == key), None)
        if match is None:
            return ()
        selected.append(match)
    return tuple(selected)


def bucket(values, domain):
    encoded = json.dumps([domain, values], separators=(',', ':')).encode()
    return int.from_bytes(hashlib.sha256(encoded).digest()[:4], 'little') % 11


def partition_model(seed=1515, iterations=1000):
    rng = random.Random(seed)
    checks = positive = 0
    for _ in range(iterations):
        upstream = tuple(rng.randrange(4) for _ in range(rng.randrange(1, 4)))
        translated = [None if rng.randrange(4) == 0 else rng.randrange(4) for _ in range(rng.randrange(1, 7))]
        preserved = select_key(upstream, translated)
        if not preserved:
            assert any(key not in translated for key in upstream)
            checks += 1
            continue
        positive += 1
        rows = [[None if rng.randrange(5) == 0 else rng.randrange(7) for _ in range(4)] for _ in range(48)]
        for domain in ('default', 'column-source'):
            placements = {}
            for index, row in enumerate(rows):
                values = [row[key] if key is not None else index % 3 for key in translated]
                physical = bucket([row[key] for key in upstream], domain)
                advertised = bucket([values[key] for key in preserved], domain)
                assert physical == advertised
                key = tuple(values)
                assert key not in placements or placements[key] == physical
                placements[key] = physical
                checks += 2
    return {'seed': seed, 'iterations': iterations, 'positive_proofs': positive,
            'placement_contract_checks': checks, 'partition_model_pass': True}


def main():
    count = 0
    for n in (8, 24, 61):
        for case, query in native.recipes(n).items():
            schema, seeds = native.schema_and_seeds(n, case)
            assert query and schema and seeds
            assert native.metadata(case, n)['peak_native_concurrency'] == 1
            expected = native.expected_rows(case, n)
            assert native.check_rows(case, expected, n)['semantic_oracle_pass']
            wrong = list(expected)
            if wrong:
                wrong[0] = dict(wrong[0], id=-17)
            else:
                wrong = [{'id': -17}]
            assert not native.check_rows(case, wrong, n)['semantic_oracle_pass']
            count += 4
    shape_checks = 0
    for case in native.recipes(61):
        if case == 'q15_keyless_control':
            expected = 0
        elif case == 'q15_source_subset':
            expected = 0
        elif case in ('q15_different_partition', 'q15_disabled_control', 'q15_override_disabled',
                      'q15_computed_key_control', 'q15_subset_keys_control'):
            expected = 2
        else:
            expected = 1
        positive = case not in ('q15_keyless_control', 'q15_different_partition', 'q15_disabled_control',
                                'q15_override_disabled', 'q15_computed_key_control', 'q15_subset_keys_control',
                                'q15_source_missing_key_control', 'q15_source_disabled_source_control')
        before = windows.physical_ast_summary(abstract_plan(expected + int(positive), filtering=True))
        assert native.check_physical_ast(case, abstract_plan(expected, filtering=True), before)['structural_oracle_pass']
        assert not native.check_physical_ast(case, abstract_plan(expected + 1, filtering=True), before)['structural_oracle_pass']
        shape_checks += 2
    cpp_checks = 0
    for case in cpp.cases():
        source = cpp.source(case)
        assert source.count(b'int main()') == 1
        assert b'GetWindowPreservedShuffling' in source and b'ComputeMetadata' in source
        assert cpp.metadata(case)['generated_source_sha256'] == hashlib.sha256(source).hexdigest()
        cpp_checks += 3
    evidence = {'native_generator_cases': len(native.recipes(61)), 'native_generator_contract_checks': count,
        'physical_checker_contract_checks': shape_checks, 'cpp_contract_cases': len(cpp.cases()),
        'cpp_generator_contract_checks': cpp_checks, 'model': partition_model(),
        'module_sha256': {Path(module.__file__).name: hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()
                          for module in (native, cpp)},
        'native_runs': 0, 'cpp_compiles': 0, 'production_edited': False, 'generated_inputs_saved': False}
    (ROOT / 'verification' / 'q15-subset-artifact-contracts.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, sort_keys=True))


if __name__ == '__main__':
    main()
