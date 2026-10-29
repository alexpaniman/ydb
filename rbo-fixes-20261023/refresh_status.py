"""Refresh the readable index from individual case records."""
import collections
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
manifest_path = HERE / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
lines = [
    'RBO fix series: 79 findings; correctness C01-C50, then optimization quality Q01-Q29.',
    'Pinned main base: ' + manifest['base'],
    'Prepared branches require verification. Guard-only and limited checks are identified explicitly.',
    '',
]
counts = collections.Counter()
followup_counts = collections.Counter()
status_labels = {
    'verified': 'Semantic checks passed',
    'verified_scoped': 'Fix-specific semantic checks passed; separate failing controls remain tracked',
    'sql_checks_passed_direct_pending': 'SQL semantic/guard checks passed; direct local contracts and scope review pending',
    'sql_and_direct_passed_scope_pending': 'SQL and direct local checks passed; shared-field scope verification pending',
    'verified_guard': 'Guard checks passed; see validation limits',
    'verified_new_parser_invariant': 'New-parser invariant checks passed; compatibility limit remains',
    'prepared_unverified': 'Prepared; build and runtime verification pending',
    'partial_prepared_unverified': 'Partial fix prepared; remaining scope and verification open',
    'prepared_needs_repair': 'Prepared patch needs repair',
    'verification_in_progress': 'Verification in progress',
    'built_pending_verification': 'Build passed; runtime verification pending',
    'verification_waiting_for_memory': 'Verification waiting for memory headroom',
    'verification_harness_failed': 'Verification harness failed; no accepted runtime verdict',
    'native_failed': 'Runtime verification failed; repair required',
    'build_failed': 'Build failed; diagnosis or repair required',
    'pending': 'Not yet prepared',
    'verified_quality': 'Semantics and optimization evidence passed',
    'partial_verified_quality': 'Implemented scope verified; remaining optimization work open',
    'verified_semantics_quality_pending': 'Semantics passed; optimization evidence pending',
}
followups = []
for path in sorted((HERE / 'cases').glob('F*.json')):
    followup = json.loads(path.read_text())
    followups.append({key: followup[key] for key in ('id', 'title', 'status', 'branch', 'commit')})
manifest['followup_items'] = followups
for item in manifest['items'] + followups:
    path = HERE / 'cases' / (item['id'] + '.json')
    case = json.loads(path.read_text()) if path.exists() else {}
    for key in ('status', 'branch', 'commit'):
        if key in case:
            item[key] = case[key]
    (followup_counts if item['id'].startswith('F') else counts)[item['status']] += 1
    if item['id'] == 'C01':
        lines.append('CORRECTNESS')
    elif item['id'] == 'Q01':
        lines.extend(['', 'OPTIMIZATION QUALITY'])
    elif followups and item['id'] == followups[0]['id']:
        lines.extend(['', 'ADDITIONAL CORRECTNESS FINDINGS FROM VERIFICATION'])
    lines.extend([item['id'] + ' | ' + item['title'], '  Status: ' + status_labels.get(item['status'], item['status'])])
    if item.get('branch'):
        lines.append('  Branch: ' + item['branch'] + ' (' + item['commit'][:12] + ')')
    native = case.get('native_verification')
    if native:
        validation = str(native['checks']) + ' targeted checks; '
        validation += 'passed' if native.get('checks_passed') else 'acceptance remains open'
        if case.get('provenance_reverification_required'):
            validation += '; exact executable receipt rerun pending'
        elif case.get('build_verification', {}).get('build_receipt'):
            validation += '; tied to the branch executable'
        lines.append('  Validation: ' + validation)
    if case.get('verification_limitation'):
        lines.append('  Verification limit: ' + case['verification_limitation'])
    scoped = case.get('scoped_native_verification')
    if scoped:
        lines.append('  Scoped validation: ' + str(scoped['distinct_passing_checks']) +
                     ' distinct fix-specific checks passed; ' + str(scoped['separate_failing_controls']) +
                     ' controls fail in the separately tracked ' + scoped['separate_finding'] + '.')
    proposal_path = HERE / 'proposed-quality-patches' / (item['id'] + '.json')
    if item['id'].startswith('Q') and proposal_path.exists() and not item.get('commit'):
        proposal = json.loads(proposal_path.read_text())
        lines.append('  Proposed patch: ' + proposal.get('status', 'unverified') + '; no verified branch yet')
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
(HERE / 'fix-status.txt').write_text('\n'.join(lines) + '\n')
state_path = HERE / 'work-state.json'
state = json.loads(state_path.read_text())
state['completed_verified_cases'] = [x['id'] for x in manifest['items'] if x['status'] == 'verified']
state['status_counts'] = dict(counts)
state['followup_status_counts'] = dict(followup_counts)
state_path.write_text(json.dumps(state, indent=2) + '\n')
print(json.dumps(dict(counts), sort_keys=True))
