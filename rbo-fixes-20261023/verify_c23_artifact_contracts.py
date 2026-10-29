"""Check C23 source contracts and independent in-memory grouping models."""
from collections import defaultdict
import hashlib
import itertools
import json
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'rbo-review-20261021'))
import null_grouping_checks as native
import prepare_c23_null_hash_key_amendment as patch


def canonical(rows):
    return sorted(json.dumps(row, sort_keys=True) for row in rows)


def oracle(rows):
    groups = defaultdict(list)
    for key, value in rows:
        groups[key].append(value)
    return [(key, len(values), sum(value is not None for value in values),
             sum(value or 0 for value in values)) for key, values in groups.items()]


def encoded_phases(rows, null_columns, partitions):
    partial = []
    for partition in range(partitions):
        states = {}
        for position, (key, value) in enumerate(rows):
            if position % partitions != partition:
                continue
            encoded = tuple(0 if index in null_columns else item for index, item in enumerate(key))
            state = states.setdefault(encoded, [0, 0, 0])
            state[0] += 1
            state[1] += value is not None
            state[2] += value or 0
        for encoded, state in states.items():
            logical = tuple(None if index in null_columns else item for index, item in enumerate(encoded))
            partial.append((logical, state))
    final = {}
    for key, partial_state in partial:
        encoded = tuple(0 if index in null_columns else item for index, item in enumerate(key))
        state = final.setdefault(encoded, [0, 0, 0])
        for index in range(3):
            state[index] += partial_state[index]
    return [(tuple(None if index in null_columns else item for index, item in enumerate(encoded)), *state)
            for encoded, state in final.items()]


def main():
    before_after = patch.render_files()
    source = before_after[patch.CPP][1]
    contracts = [
        'type->GetKind() == ETypeAnnotationKind::Null',
        'Ctx.NewCallable(Pos, "Uint8", {Ctx.NewAtom(Pos, "0")})',
        'Ctx.NewCallable(Pos, "Null", {})',
        'addResult(finishKey(traits.OriginalColName), traits.ResultColName);',
        'addResult(finishKey(key), key);',
        'if (IsScalarAggregation() && aggregationPhase != EOpPhase::Intermediate)',
    ]
    assert all(contract in source for contract in contracts)
    unchanged = ['BuildInitHandlerLambda', 'BuildUpdateHandlerLambda', 'BuildPhysicalAggregationTraits', 'GetKeyFields']
    for name in unchanged:
        # The patch never changes these method bodies or any input-field naming.
        old, new = before_after[patch.CPP]
        signature = 'TPhysicalAggregationBuilder::' + name + '('
        start_old, start_new = old.index(signature), new.index(signature)
        body_old = old[start_old:old.index('\n}\n', start_old) + 3]
        body_new = new[start_new:new.index('\n}\n', start_new) + 3]
        assert body_old == body_new
    randomizer = random.Random(2301026)
    model_cases = 0
    for width in range(1, 5):
        for mask in range(1, 1 << width):
            null_columns = {index for index in range(width) if mask & (1 << index)}
            for size, partitions, null_argument in itertools.product((0, 1, 7, 31), (1, 2, 5), (False, True)):
                rows = [(tuple(None if index in null_columns else randomizer.choice((None, -1, 0, 2))
                               for index in range(width)),
                         None if null_argument else randomizer.choice((None, -3, 0, 5)))
                        for _ in range(size)]
                assert canonical(oracle(rows)) == canonical(encoded_phases(rows, null_columns, partitions))
                model_cases += 1
    # Independent invariants reject the three tempting but incorrect shortcuts.
    assert oracle([]) != [((), 0, 0, 0)]  # deleting the last key must not create a scalar empty row
    assert canonical(oracle([((None,), None)])) != canonical([((0,), 1, 0, 0)])
    assert canonical(oracle([((None,), None)])) != canonical([((None,), 1, 1, 0)])
    generated = 0
    for size in (1, 2, 11, 24):
        queries = native.recipes(size)
        for case in native.specs():
            assert queries[case]
            rows = native.expected_rows(case, size)
            assert native.check_rows(case, list(reversed(rows)), size)
            assert not native.check_rows(case, rows + [{'unexpected': True}], size)
            generated += 3
    target = ROOT / 'proposed-correctness-amendments/C23-null-hash-key.patch'
    report = {'id': 'C23', 'patch_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
              'source_contracts': len(contracts) + len(unchanged), 'grouping_model_cases': model_cases,
              'mutation_classes_rejected': 3, 'native_generator_cases': len(native.specs()),
              'generator_contracts': generated, 'compiled': False, 'native_verified': False,
              'production_edited': False, 'generated_payloads_saved': False,
              'source_diagnosis': {'file': 'ydb/library/yql/dq/comp_nodes/dq_hash_operator_serdes.cpp',
                                   'line': 116, 'row_and_block_share_key_type_constraint': True},
              'pending': ['Native compilation and both row/block engine runs.',
                          'Distributed shuffle and both aggregate phases on the amended branch.']}
    (ROOT / 'verification/C23-null-hash-key-artifact-contracts.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
