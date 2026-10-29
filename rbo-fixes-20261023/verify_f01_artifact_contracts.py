"""F01 source/identity checks only; no rendered workload is written."""
import hashlib
import itertools
import json
from pathlib import Path

import prepare_f01_distinct_alias_patch as patch

ROOT = Path(__file__).resolve().parent


def reuse(entry, current):
    return (current['direct_row'] and not entry['force_optional']
            and entry['name'] == current['name'] and entry['direct_row']
            and entry['source'] == current['source'])


def main():
    before, source = patch.render_files()[patch.SOURCE]
    contracts = (
        'member.Struct().Ptr() == lambda.Args().Arg(0).Ptr()',
        'forceOptional || name.GetFullName() != originalColName.GetFullName()',
        'previous.Body().Maybe<TCoMember>()',
        'access.Cast().Struct().Ptr() == previous.Args().Arg(0).Ptr()',
        'access.Cast().Name().Value() == member.Name().Value()',
        'if (!alreadyAdded)',
    )
    assert all(item in source for item in contracts)
    traits = '        const auto distinctAggTraits = BuildAggregationTraits(originalColName.GetFullName(), "distinct", resultColName, ctx, pos);'
    assert before[before.index(traits):] == source[source.index(traits):]
    assert set(patch.render_files()) == {patch.SOURCE}
    identities = 0
    unsafe_name_only = 0
    for same_name, same_source, previous_row, current_row, force_optional in itertools.product((False, True), repeat=5):
        previous = dict(name='n', source='input', direct_row=previous_row, force_optional=force_optional)
        current = dict(name='n' if same_name else 'm', source='input' if same_source else 'other', direct_row=current_row)
        expected = same_name and same_source and previous_row and current_row and not force_optional
        assert reuse(previous, current) == expected
        unsafe_name_only += same_name and not expected
        identities += 1
    assert unsafe_name_only > 0
    layout_cases = 0
    for count in (2, 3, 5, 12):
        definitions = []
        traits_count = 0
        for _ in range(count):
            current = dict(name='n', source='input', direct_row=True, force_optional=False)
            if not any(reuse(previous, current) for previous in definitions):
                definitions.append(current)
            traits_count += 1
        assert len(definitions) == 1 and traits_count == count
        layout_cases += 1
    target = ROOT / 'proposed-correctness-amendments/F01-distinct-input-dedup.patch'
    evidence = {'id': 'F01', 'base': patch.BASE, 'patch_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
                'source_contracts': len(contracts) + 2, 'identity_gate_combinations': identities,
                'name_only_reuse_counterexamples': unsafe_name_only, 'alias_layout_cases': layout_cases,
                'importer_changed': False, 'production_edited': False, 'compiled': False,
                'native_verified': False, 'generated_payloads_saved': False,
                'diagnosis': 'Direct-column DISTINCT aliases append duplicate same-name input definitions before a projecting Map; result traits are separate and must remain separate.',
                'not_claimed': ['General non-projecting Map shadowing semantics.',
                                'Conflicting distinct expressions that accidentally choose the same generated name.',
                                'Bare-Null physical hash support on the standalone F01 branch.']}
    (ROOT / 'verification/F01-distinct-alias-artifact-contracts.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence))


if __name__ == '__main__':
    main()
