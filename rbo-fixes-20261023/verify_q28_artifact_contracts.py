"""Check Q28 model and generator/source contracts without writing generated IR."""
import hashlib
import json
from pathlib import Path

import prepare_q28_local_worklist_amendment as amendment
import quality_scheduler_cpp_checks as cpp
import quality_scheduler_model as model


ROOT = Path(__file__).resolve().parent


def digest(content):
    return hashlib.sha256(content).hexdigest()


def verify():
    generated = cpp.render_cpp_suite()
    specifications = cpp.specs()
    assert len(specifications) == len(set(cpp.cases())) == 26
    assert generated.count('int main()') == 1
    assert generated.count('case_index') == len(specifications)
    assert generated.count('checks_executed') == len(specifications)
    assert generated.count('"passed"') == 0  # JSON field appears C++-escaped.
    assert 'LocalRewriteDepth' not in generated  # One suite also compiles on pinned main.
    assert 'RecomputeLocalOutputIUs' not in generated
    assert all(spec['branches'] * spec['depth'] * 3 < 1000 for spec in specifications)
    expected_modes = {'kept_map', 'removed_map', 'join_children', 'mixed_chain',
                      'canonical_replicate', 'raw_shared_slots', 'subplan_root',
                      'cbo_exclusion', 'physical_exclusion', 'parents_exclusion',
                      'metadata_exclusion', 'types_exclusion', 'liveness_exclusion'}
    assert {spec['mode'] for spec in specifications if spec.get('scenario')} == expected_modes
    for counter in ('Refreshes', 'MatchesTried', 'Visits'):
        assert 'local.' + counter + ' == conservative.' + counter in generated
        assert 'local.' + counter + ' < conservative.' + counter in generated
    source = amendment.render_files()
    rbo = source[amendment.PREFIX + 'kqp_rbo.cpp'][1]
    contract = source[amendment.PREFIX + 'kqp_rbo.h'][1]
    rules = source[amendment.PREFIX + 'kqp_rbo_rules.h'][1]
    checks = {
        'unknown_effect_defaults_to_conservative': 'LocalRewriteDepth() const { return std::nullopt; }' in contract,
        'shared_producer_boundary_documented': "change a shared producer's output IDs" in contract,
        'right_join_trace_behavior_preserved': 'ISimplifiedRule("Rewrite right join", 0)' in rules,
        'only_three_rules_opt_in': rules.count('LocalRewriteDepth() const override') == 3,
        'only_output_property_admitted': '~ui32(ERuleProperties::RequireOutputIUs)' in rbo,
        'owning_slots_checked': 'it->second == &slot' in rbo,
        'cbo_and_physical_excluded': 'item.Current->Kind != EOperator::CBOTree && !item.Current->Props.StageId' in rbo,
        'canonical_subplan_slot_used': 'root.PlanProps.Subplans.MutablePlan(*entry.SubplanIU)' in rbo,
        'child_first_local_refresh': 'changed.Current->RecomputeLocalOutputIUs()' in rbo,
        'matched_root_output_contract_checked': 'op->GetOutputIUs() == previousOutputs' in rbo,
        'dirty_region_requeued': 'work.insert(work.end(), region.rbegin(), region.rend())' in rbo,
        'fresh_conservative_pass_retained': 'computedProps = 0;\n        }\n    }\n\n    while (fired && numMatches < maxNumOfMatches)' in rbo,
        'match_ceiling_retained': 'Y_ENSURE(numMatches < maxNumOfMatches)' in rbo,
    }
    assert all(checks.values())
    evidence = {'finding': 'Q28', 'base': amendment.BASE,
                'patch_sha256': digest((ROOT / 'proposed-quality-amendments/Q28-local-worklist.patch').read_bytes()),
                'artifact_contracts_passed': True, 'source_contracts': checks,
                'cpp_generator': cpp.metadata(), 'model': model.verify(),
                'generator_sha256': digest((ROOT / 'quality_scheduler_cpp_checks.py').read_bytes()),
                'model_sha256': digest((ROOT / 'quality_scheduler_model.py').read_bytes()),
                'compiled': False, 'native_runs': 0, 'generated_source_saved': False,
                'production_edited': False, 'status': 'architectural_step_prepared_partial_unverified',
                'limits': ['Model equivalence is not native equivalence.',
                           'Native generator assertions have not been compiled or run.',
                           'Unreviewed rules and global property analyses retain conservative scheduling.',
                           'Right-join semantic edge cases use the existing separate Q28 acceptance profile.']}
    path = ROOT / 'verification/q28-local-worklist-artifact-contracts.json'
    path.write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'artifact_contracts_passed': True, 'cpp_cases': len(specifications),
                      'source_contracts': len(checks), 'model_pass': evidence['model']['model_pass'],
                      'patch_sha256': evidence['patch_sha256'], 'native_runs': 0}))


if __name__ == '__main__':
    verify()
