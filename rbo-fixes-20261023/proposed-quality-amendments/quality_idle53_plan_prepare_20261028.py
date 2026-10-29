"""Prepare a source-only launch plan; this file never launches its commands."""
from pathlib import Path
import copy
import hashlib
import json
import shlex

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


old_path = ROOT / 'verification/quality-all29-serial-execution-plan-20261028.json'
composition_path = ROOT / 'verification/quality-idle53-source-composition-20261028.json'
fingerprint_path = ROOT / 'verification/quality-idle53-profile-fingerprints-20261028.json'
assert sha(old_path) == '43bf967ad02bfa9e10fecf08066c14514280c7a46805811366a2a3aee93c0ceb'
assert sha(composition_path) == '1bac7bb6118969b3d1b0c11fcd0215a3a1974ff66555250ffea4b4edda61544f'
assert sha(fingerprint_path) == '59501f3e47be457a61ed690e3dbf2e53ed03457e300cb8fb7cc4db7ca4ca5cd4'
plan = json.loads(old_path.read_bytes())
composition = json.loads(composition_path.read_bytes())
fingerprints = json.loads(fingerprint_path.read_bytes())['fingerprints']
fixed = str(ROOT / 'worktrees/verify')
baseline = str(ROOT / 'worktrees/metadata')
plan['schema_version'] = 'quality_all29_serial_execution_plan_idle53_v1'
plan['status'] = 'source_only_conditional_launch_plan_not_executed'
plan['supersedes_plan_sha256'] = sha(old_path)
plan.pop('prepared_unix_ns', None)
plan['source_state']['dormant_composition'] = dict(path=str(composition_path.relative_to(ROOT)),
    sha256=sha(composition_path), intended_final_applied_count=53,
    binding_refresh_independent_review_pending=True, application_performed=False,
    optional_first_checkpoint=composition['idle_checkpoints']['after_C31_step'])
plan['source_state']['stale_state_warning'] = (
    'Snapshot was prepared from applied44. Root may independently install exact C31 as applied45 before the remaining eight patches. '
    'Use the composition per-step image checkpoints; reject mixed images and never reapply an already matching step. '
    'All native evidence and resource/full-quality qualifications remain unchanged by preparation.')
plan['source_state']['dormant_reviewed_patches'] = [dict(item, application_status='dormant_at_plan_preparation')
    for item in plan['source_state']['dormant_reviewed_patches']]
plan['installation_order'] = [
    'Finish the root-owned correctness build/native queue and release its execution slot before any source change.',
    'Validate every before-image in quality-idle53-source-composition-20261028.json. C31 is independently reviewed and may be installed alone as ledger45 for its native37 run.',
    'At the next idle boundary, recognize the exact ledger45 checkpoint and apply only the remaining eight patches in order: Q01 named window/setup, Q28 pilot/runtime identity/version replay/read-only handoff, Q-SQL cap/revision guard, binding-only dependency refresh.',
    'The binding-only refresh and this launch plan require independent review before installation. Validate all ten final file hashes and all 28 adapter dependencies; freeze these exact sources.',
    'Resolve current profile fingerprints at canonical installed paths and require equality with quality-idle53-profile-fingerprints-20261028.json before collecting fresh quality evidence.',
    'Root may then reuse tracked-clean metadata worktree as baseline by switching it detached to pinned main; preserve aggregate worktree and do not switch verify while any process owns it.'
]
plan['worktrees'] = dict(
    baseline=dict(path=baseline, observed_head_before_future_switch='3ca6d9b548fe7865bf535c0d1a646bbfa788a3c1',
        tracked_clean_confirmed_by='root_read_only_source_check', required_head=plan['base_revision'],
        future_switch_required=True, switch_owner='root_after_idle', new_worktree_required=False),
    fixed=dict(path=fixed, status='occupied_by_root_correctness_queue_at_preparation',
        future_switch_owner='root_serial_driver_only_after_release'),
    preserved=dict(path=str(ROOT / 'worktrees/aggregate'), revision='24e1aae9a0677e8ed12f257a7944c50516e9cba4',
        reason='Existing C31 closure; leave unchanged.'),
    unavailable_stale_paths=['/tmp/rbo-fixes-20261023/verify', '/tmp/rbo-fixes-20261023/logical', '/tmp/rbo-fixes-20261023/windows'])

for name, digest in composition['after_images'].items():
    if name.startswith(ROOT.name + '/'):
        plan['source_bindings'][name.split('/', 1)[1]] = digest
plan['profile_after_images'] = dict(path=str(fingerprint_path.relative_to(ROOT)), sha256=sha(fingerprint_path),
    canonical_root=str(REPO), ready_only_after_exact_installation=True)
for finding in plan['findings']:
    if finding['bug'] in fingerprints:
        finding['future_installed_profile_sha256'] = fingerprints[finding['bug']]['sha256']
    if finding.get('spill_enabled_case_count'):
        finding['SQL_execution_prerequisite'] = 'exact_idle53_installation_and_28_dependency_checks_with_validated_production_revision_token'
    if finding['bug'] == 'Q01':
        finding['notes'] = [note.replace('Named-window/setup patch dormant.',
            'Named-window/setup patch is part of the dormant idle53 composition; actual frontend/path validation remains pending.')
            for note in finding['notes']]


def replace_baseline(value):
    if isinstance(value, list):
        return [replace_baseline(item) for item in value]
    if isinstance(value, dict):
        return {key: replace_baseline(item) for key, item in value.items()}
    return baseline if value == '{baseline_worktree}' else value


plan = replace_baseline(plan)
templates = plan['serial_command_templates']
old_unblocked = next(item for item in templates if item['id'] == 'canonical_SQL_unblocked_profiles')


def sql_template(identifier, bugs, count, direct=()):
    argv = ['python3', '-B', str(ROOT / 'verify_series.py'), '--worktree', fixed,
            '--quality-baseline-worktree', baseline, '--bugs', *bugs,
            '--rows', '24', '--runner-only', '--build-workers', '8']
    for bug in direct:
        argv += ['--quality-ir-evidence', '{fixed_direct_' + bug + '}']
    return dict(id=identifier, stage='SQL', argv=argv, expected_cases=count, expected_records_per_phase=count,
        precondition='Exact idle53 sources installed/frozen; root serial execution slot released; clean metadata baseline and fixed verify checkout. '
            + ('Required fixed direct evidence available and paired direct review completed where applicable.' if direct
               else 'No direct-contract prerequisite for these findings.'),
        status_limit='Each full existing quality gate decides status. Fresh complete vectors and paired semantics are required; this plan grants no admission.')


first = sql_template('first_SQL_Q08_Q02', ['Q08', 'Q02'], 6)
statistics = sql_template('next_SQL_Q09_Q20', ['Q09', 'Q20'], 6, ['Q09', 'Q20'])
union = sql_template('next_SQL_Q11', ['Q11'], 11, ['Q11'])
remaining = sql_template('remaining_SQL_without_spill_cells', ['Q05', 'Q12', 'Q13', 'Q22', 'Q26'], 64)
position = templates.index(old_unblocked)
templates[position:position + 1] = [first, statistics, union, remaining]
for item in templates:
    if item['id'] in ('canonical_SQL_after_reviewed_spill_setup', 'extra_SQL_Q14', 'canonical_SQL_Q01'):
        item['execution_blocked'] = True
        item['unblocks_when'] = 'Root installs independently approved exact idle53 composition and verifies its final hashes; no runtime privacy evidence is assumed.'
        item['precondition'] = item['precondition'].replace(
            'until source-reviewed no-payload-persistence bounds/configuration are installed for every enabled spilling cell and exact generators/profiles are refrozen',
            'until the reviewed exact82-cell cap/revision-token adapter plus binding-only refresh are installed and the final profile hashes match')
        item['precondition'] += ' Cap rejection is a failed query. Preserve enabled query/channel and block/hash-shuffle settings; only reviewed node mask and cap differ.'
plan['first_workload_sequence'] = [
    dict(step=1, template='first_SQL_Q08_Q02', findings=['Q08', 'Q02'],
         rationale='Six canonical cases per phase, no spilling cells or direct admission requirement, and complete existing quality gates.',
         expected_target_records=12, expected_legacy_preflight_records=4,
         runner_builds=dict(baseline=1, independent_fixed=2)),
    dict(step=2, templates=['baseline_rbo_prepare', 'baseline_direct_Q09', 'baseline_direct_Q20',
         'fixed_select_Q09', 'fixed_prepare_Q09', 'fixed_direct_Q09', 'pair_direct_Q09',
         'fixed_select_Q20', 'fixed_prepare_Q20', 'fixed_direct_Q20', 'pair_direct_Q20', 'next_SQL_Q09_Q20'],
         direct_cases_per_phase=dict(Q09=38, Q20=72), SQL_cases_per_phase=6,
         rationale='Exercise real fixed-direct admission plumbing with one reusable validated baseline RBO closure.'),
    dict(step=3, templates=['fixed_select_Q11', 'fixed_prepare_Q11', 'fixed_direct_Q11', 'next_SQL_Q11'],
         fixed_direct_cases=19, SQL_cases_per_phase=11),
    dict(step=4, template='remaining_SQL_without_spill_cells', SQL_cases_per_phase=64,
         rationale='The remaining no-spill canonical findings can share one pinned-main runner build.'),
]
plan['baseline_reuse']['SQL'] = (
    'verify_series.prepare_quality_baseline builds pinned main once per invocation; no reuse-baseline-evidence CLI exists. '
    'The recommended first invocation is Q08/Q02, followed by Q09/Q20 after their direct pairs, Q11 after its fixed19, then the remaining five no-spill findings. '
    'This splits the former ten-finding87-case batch into6+6+11+64 without dropping or duplicating cells. '
    'The reviewed82-cell adapter conditionally enables the twelve-finding97-case canonical batch, Q14extra6 and Q01canonical652 after installation. '
    'Q25extra8/11 remains separate; Q28 SDK shared cell stays excluded. Separate metadata baseline from rotating verify checkout.')
plan['admission_order'][0] = (
    'The82 affected SQL cells across14 findings are source-reviewed with a1-byte enabled local-service cap and node mask None. '
    'Do not execute them until the exact idle53 adapter, production revision token, 28 dependency bindings and final profiles are installed and verified. '
    'This is no-payload-write source evidence only; cap rejection stays a query failure and there is no actual spill occurrence claim.')
plan['admission_order'][1] = (
    'Run the actual fixed local runtime/IR contract before the corresponding SQL finding and paired local evidence where supported. '
    'Independent Q08/Q02 need no direct prerequisite; do not wait for every other finding direct suite to start them. '
    'Current SQL validation requires fixed evidence for Q09/Q11/Q20 unconditionally, all19 Q15 cases, and239 Q01 cases.')
inventory = plan['spill_setup_inventory']
inventory['scope'] = ('Source-reviewed exact82-cell adapter pending installation and final binding-only review; paired rows24/new-mode only. '
    'Runtime spill occurrence is unknown; no small-N inference. Query/channel/block flags stay enabled. Compute mask/cap do not verify disk-spill performance or recovery.')
inventory['adapter_composition'] = str(composition_path.relative_to(ROOT))
inventory['post_install_blocked_canonical_lanes_due_only_to_payload_setup'] = 0
inventory['SDK_shared_cell_still_blocked'] = True
inventory['not_an_admission_or_helper_change'] = True
plan['special_cases']['Q01']['next_available_step'] = plan['special_cases']['Q01']['next_available_step'].replace(
    'After dormant named-window/setup patch', 'After exact idle53 installation and named-window/setup source checks')
plan['special_cases']['Q01']['call_template']['worktree'] = '{baseline_metadata_or_fixed_verify_for_exact_phase}'
plan['special_cases']['Q01']['limits'] += ' Canonical cap policy does not apply to or expand the no-spill scaling pilot.'
plan['special_cases']['Q28']['shared_spilling_control'] = 'SDK_cell_excluded_even_after_canonical_SQL_adapter_installation'
plan['special_cases']['Q28']['limits'] = (
    'Canonical SQL adapter is called only from native_verify.run_generated. SDK measurement preparation is a separate path; '
    'its shared-spilling cell is still excluded and the seven-case raw pilot cannot promote full Q28 quality.')
for path in [old_path, composition_path, fingerprint_path,
    ROOT / 'verification/Q-SQL-spill-cap-independent-root-review-20261028.json',
    ROOT / 'verification/Q-SQL-spill-revision-guard-independent-root-review-20261028.json']:
    plan['references'][str(path.relative_to(ROOT))] = sha(path)
plan['validation'].update(ordered_composition_metadata_checks=composition['verification']['checks_passed'],
    exact_SQL_vector_cells=850, exact_SQL_records_per_phase=853, first_workload_templates_added=True,
    thresholds_or_admission_policy_changes=False, first_workloads_executed=False)

# Mechanical schedule checks only; no command dispatch.
findings = {finding['bug']: finding for finding in plan['findings']}
sql_owners = []
for item in templates:
    if item['stage'] != 'SQL':
        continue
    argv = item['argv']
    bugs = argv[argv.index('--bugs') + 1:argv.index('--rows')]
    assert item['expected_cases'] == sum(findings[bug]['sql_cases'] for bug in bugs)
    assert item['expected_records_per_phase'] == sum(findings[bug]['sql_records_per_phase'] for bug in bugs)
    assert '--repetitions' not in argv
    sql_owners.extend(bugs)
assert len(sql_owners) == len(set(sql_owners)) == 25
assert sum(item['expected_cases'] for item in templates if item['stage'] == 'SQL') == 850
assert sum(item['expected_records_per_phase'] for item in templates if item['stage'] == 'SQL') == 853
assert all(findings[bug]['future_installed_profile_sha256'] == fingerprints[bug]['sha256'] for bug in sql_owners)
assert plan['special_cases']['Q01']['quality_acceptance_enabled'] is False
assert plan['special_cases']['Q28']['quality_acceptance_enabled'] is False
assert plan['special_cases']['Q25']['full_quality_acceptance_enabled'] is False

out = ROOT / 'verification/quality-all29-serial-execution-plan-idle53-20261028.json'
out.write_text(json.dumps(plan, sort_keys=True, indent=2) + '\n')
lines = [
    'Dormant quality execution plan after exact idle53 helper installation.',
    'Source-only: no native/build command in this plan has been executed.',
    'All29 findings preserved:850 SQL cases/853 records per phase; correctness remains separate.',
    'First ordinary invocation:Q08+Q02,6 cases per phase. Then Q09/Q20 after110 direct cases per phase, Q11 after fixed19.',
    'Use existing metadata worktree as future pinned-main baseline; keep aggregate unchanged and release verify from correctness first.',
    'C31 may be installed as ledger45 first. Resume from the exact checkpoint; final53 contains nine new patches including two-line binding refresh.',
    'All82 spill-enabled SQL cells stay conditional on exact cap/revision-token/binding installation. Cap rejection remains failure.',
    'Q01 resource/full-quality and Q28 SDK shared/timing/full-quality gates remain pending; no thresholds changed.',
    'No --repetitions override:Q25 volatile control retains4 repetitions.',
    'Baseline prepare/select must precede direct use; all execution is serial under existing guard.',
    '', 'First workload command (only after root releases the queue and verifies final sources):',
    shlex.join(first['argv']), '', 'Next SQL commands require the corresponding real fixed direct evidence paths:',
    shlex.join(statistics['argv']), shlex.join(union['argv']), '',
    'Remaining concrete commands, exact vectors, review hashes and image checkpoints are in the JSON plan.',
    'JSON SHA256: ' + sha(out),
]
text_path = out.with_suffix('.txt')
text_path.write_text('\n'.join(lines) + '\n')
print(json.dumps(dict(plan=str(out.relative_to(ROOT)), sha256=sha(out), text_sha256=sha(text_path),
                      SQL_cases=850, SQL_records=853, first_cases_per_phase=6)))
