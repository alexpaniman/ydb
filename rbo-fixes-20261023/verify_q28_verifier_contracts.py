"""Mock only dormant Q28 evidence handling; no subprocess or compiler starts."""
import copy
import hashlib
import json
from pathlib import Path

import anonymous_cpp_verify as verify
import quality_scheduler_cpp_checks as generator

ROOT = Path(__file__).resolve().parent


def evidence(phase, case):
    fixed = phase == 'fixed'
    records = []
    for index, spec in enumerate(generator.specs()):
        excluded = spec['unknown_control'] or spec['mode'].endswith('_exclusion') or spec['mode'] == 'raw_shared_slots'
        count = 10 if excluded or not fixed else 2
        record = {'case_index': index, 'checks_executed': 1, 'passed': fixed or excluded}
        for name in ('output_refreshes', 'rule_attempts', 'candidate_visits'):
            record[name] = count
            record['control_' + name] = 10 if spec.get('scenario') else 0
        records.append(record)
    digest = '0' * 64
    result = {'schema_version': 'anonymous_direct_cpp_v1', 'bug': 'Q28', 'phase': phase,
              'revision': case['commit'] if fixed else case['base'], 'closure_provenance_verified': True,
              'cases': records, 'generated_files_persisted': False,
              'generator_module': verify.GENERATORS['Q28'], 'case_count': len(records),
              'proof_scope': 'linked_production_local_contracts',
              'generator_sha256': hashlib.sha256(Path(generator.__file__).read_bytes()).hexdigest(),
              'generated_source_sha256': hashlib.sha256(generator.render_cpp_suite().encode()).hexdigest(),
              'closure_receipt_sha256': digest,
              'generated_object': {'sha256': digest, 'size_bytes': 1},
              'generated_executable': {'sha256': digest, 'size_bytes': 1}}
    for name in ('compile', 'link', 'native'):
        result[name] = {'cleanup_complete': True, 'stdout_overflow': False,
                        'exit_code': 1 if name == 'native' and not fixed else 0}
    return result


def main():
    case = json.loads((ROOT / 'cases/Q28.json').read_text())
    before, after = evidence('baseline', case), evidence('fixed', case)
    source, count, digest = verify.render_suite('Q28', ROOT.parent, case['base'])
    assert count == len(generator.specs()) == 26
    assert source == generator.render_cpp_suite().encode()
    assert digest == hashlib.sha256(Path(generator.__file__).read_bytes()).hexdigest()
    result = verify.pair_direct('Q28', before, after, case)
    assert result['local_contract_improvement_verified'] and not result['quality_finding_fully_verified']
    assertions = 4
    excluded = next(index for index, spec in enumerate(generator.specs()) if spec['mode'] == 'raw_shared_slots')
    changed = copy.deepcopy(after)
    changed['cases'][excluded]['candidate_visits'] += 1
    assert not verify.pair_direct('Q28', before, changed, case)['local_contract_improvement_verified']
    assertions += 1
    for field in ('output_refreshes', 'rule_attempts', 'candidate_visits',
                  'control_output_refreshes', 'control_rule_attempts', 'control_candidate_visits'):
        for mode in ('missing', 'negative', 'boolean'):
            changed = copy.deepcopy(after)
            if mode == 'missing':
                del changed['cases'][0][field]
            else:
                changed['cases'][0][field] = -1 if mode == 'negative' else False
            try:
                verify.pair_direct('Q28', before, changed, case)
            except verify.MetadataError:
                assertions += 1
            else:
                raise AssertionError('Invalid scheduler counter evidence accepted')
    changed = copy.deepcopy(after)
    changed['cases'][0]['unapproved_payload'] = 'synthetic'
    try:
        verify.pair_direct('Q28', before, changed, case)
    except verify.MetadataError:
        assertions += 1
    else:
        raise AssertionError('Unapproved evidence field accepted')
    for field in ('generator_sha256', 'generated_source_sha256'):
        changed_before, changed_after = copy.deepcopy(before), copy.deepcopy(after)
        changed_before[field] = changed_after[field] = '1' * 64
        try:
            verify.pair_direct('Q28', changed_before, changed_after, case)
        except verify.MetadataError:
            assertions += 1
        else:
            raise AssertionError('Stale scheduler specifications accepted')
    changed_before = copy.deepcopy(before)
    changed_before['cases'][0]['output_refreshes'] = 0
    try:
        verify.pair_direct('Q28', changed_before, after, case)
    except verify.MetadataError:
        assertions += 1
    else:
        raise AssertionError('Baseline semantic-failure sentinel accepted')
    run_source = source.decode().split('static void Run(ui32', 1)[1].split("'''", 1)[0]
    published = run_source.index('ReportRefreshes = refreshes;')
    second_run = run_source.index('const auto finalAttempts = Attempts;')
    assert published > second_run
    assert run_source[:published].count('NTests::AssertIdInvariants') == 2
    assertions += 2
    metadata = {'mock_contracts_passed': True, 'assertions': assertions, 'case_count': count,
                'generator_sha256': digest, 'verifier_sha256': hashlib.sha256(Path(verify.__file__).read_bytes()).hexdigest(),
                'generated_source_sha256': hashlib.sha256(source).hexdigest(),
                'compiled': False, 'native_runs': 0, 'production_edited': False,
                'generated_source_saved': False, 'synthetic_evidence_saved': False}
    (ROOT / 'verification/q28-dormant-verifier-contracts.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
