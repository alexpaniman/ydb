"""Sequential independent-commit verification. Run only during a source freeze."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from verification_contract import record_verdict, summarize_verification
from memory_safe_build import run_build
from quality_pairing import PROFILES, generator_modules, pair_quality_records, profile_fingerprint, selected_cases
from anonymous_cpp_guard import run_anonymous

HERE = Path(__file__).resolve().parent


def emit(value):
    (HERE / 'verification' / 'active-series.json').write_text(
        json.dumps(dict(value, updated_unix_time=time.time()), indent=2) + '\n')
    print(json.dumps(value, sort_keys=True), flush=True)


def require_clean(worktree):
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=worktree):
        raise SystemExit('Verification worktree has tracked changes')


def native_command(worktree, phase, bugs, rows, modules=(), repetitions=None, *, q01_sample=None):
    modes = ['new', 'legacy'] if len(bugs) == 1 and bugs[0] in ('C37', 'C38') else ['new']
    command = [sys.executable, str(HERE / 'native_verify.py'), '--worktree', str(worktree),
               '--phase', phase, '--bugs', *bugs, '--rows', str(rows), '--modes', *modes]
    for module in modules:
        command += ['--generator-module', module]
    if repetitions is not None:
        command += ['--repetitions', str(repetitions)]
    if q01_sample is not None:
        from quality_q01_scaling_checks import validate_sample
        sample = validate_sample(q01_sample)
        if (bugs != ['Q01'] or list(modules) != ['quality_q01_scaling_checks']
                or repetitions != 1 or rows != sample['rows']):
            raise ValueError('Q01 measurement requires its dedicated one-cell profile')
        command += ['--measure-q01-child-resources', '--cases', sample['case'],
                    '--q01-run-id', sample['run_id'], '--q01-sample-kind', sample['kind'],
                    '--q01-sample-index', str(sample['index'])]
    return command


def invoke_native(worktree, phase, bugs, revision, rows, modules=(), repetitions=None, *, q01_sample=None):
    # As for ordinary batches, the caller must hold the existing global serial
    # execution lease. This opt-in runs one cell and never starts a build.
    paths = {bug: HERE / 'verification' / (phase + '-' + bug + '-' + revision[:12] + '.jsonl') for bug in bugs}
    prior = {bug: len(path.read_text().splitlines()) if path.exists() else 0 for bug, path in paths.items()}
    command = (native_command(worktree, phase, bugs, rows, modules, repetitions, q01_sample=q01_sample)
               if q01_sample is not None else native_command(worktree, phase, bugs, rows, modules, repetitions))
    stamp = str(time.time_ns())
    memory_path = HERE / 'verification' / ('native-memory-' + phase + '-' + '-'.join(bugs) + '-' + revision[:12] + '-' + stamp + '.json')
    # The scope contains the entire Python batch and all legacy generator
    # subprocesses. Inner query timeouts still apply independently. The broad
    # batch deadline accommodates the expanded Q01 matrix at its worst-case
    # per-query timeout without bypassing memory limits between cases.
    memory, stdout = run_anonymous(command, Path.cwd(), {}, timeout=48 * 3600 * max(1, repetitions or 1),
                                   max_scope_gib=24, min_host_gib=36,
                                   retain_stdout=256 * 1024, compiler_environment=False,
                                   metrics_path=memory_path)
    guard_pass = (memory.get('exit_code') == 0 and memory.get('cleanup_complete') is True
                  and not memory.get('watchdog_stopped'))
    if q01_sample is not None:
        guard_pass = (guard_pass and type(memory.get('exit_code')) is int
                      and memory.get('watchdog_stopped') is False and memory.get('stdout_overflow') is False)
    memory_path.write_text(json.dumps(memory, sort_keys=True, indent=2) + '\n')
    native = subprocess.CompletedProcess(command, memory['exit_code'] if guard_pass else memory.get('exit_code') or 75,
                                         stdout.decode(errors='replace'), '')
    records = {bug: [json.loads(line) for line in path.read_text().splitlines()[prior[bug]:]] if path.exists() else []
               for bug, path in paths.items()}
    guarded_paths = {}
    memory_digest = hashlib.sha256(memory_path.read_bytes()).hexdigest()
    for bug, values in records.items():
        for value in values:
            value.update(native_batch_guard_pass=guard_pass,
                         native_batch_memory_evidence=str(memory_path), native_batch_memory_sha256=memory_digest)
            if q01_sample is not None and 'q01_child_observation' in value:
                value['q01_child_observation'].update(outer_memory_guard_admission_pending=False,
                    outer_memory_guard_verified=guard_pass, outer_memory_guard_sha256=memory_digest)
                if not guard_pass:
                    value['q01_child_observation']['status'] = 'outer_guard_rejected'
        guarded_paths[bug] = HERE / 'verification' / ('guarded-' + phase + '-' + bug + '-' + revision[:12] + '-' + stamp + '.jsonl')
        guarded_paths[bug].write_text(''.join(json.dumps(value, sort_keys=True) + '\n' for value in values))
    return native, guarded_paths, records


def prepare_quality_baseline(args, state, modules_by_bug):
    """Build pinned main once, then run the selected quality profiles serially."""
    if not modules_by_bug or args.build_only:
        return {}, None, None
    worktree = args.quality_baseline_worktree or args.worktree
    require_clean(worktree)
    base = state['base']
    resolved = subprocess.check_output(['git', 'rev-parse', base], cwd=worktree, text=True).strip()
    if resolved != base:
        raise SystemExit('Quality baseline must be the pinned full main revision')
    subprocess.run(['git', 'switch', '--detach', base], cwd=worktree, check=True, capture_output=True)
    emit({'step': 'quality_baseline_build_started', 'revision': base})
    start = time.monotonic()
    target = 'ydb/tests/tools/kqprun'
    build, memory = run_build(worktree, target, args.build_workers)
    record = {'revision': base, 'exit_code': build.returncode, 'target': target, 'memory': memory,
              'elapsed_seconds': round(time.monotonic() - start, 1),
              'diagnostic_sha256': hashlib.sha256((build.stdout + build.stderr).encode()).hexdigest()}
    path = HERE / 'verification' / ('quality-baseline-build-' + base[:12] + '.json')
    path.write_text(json.dumps(record, indent=2) + '\n')
    if build.returncode:
        emit({'step': 'quality_baseline_build_failed', **record})
        # No fixed branch has been tested or assigned a failure by this step.
        return None, None, 1
    from binary_provenance import write_build_receipt
    record['build_receipt'] = str(write_build_receipt(worktree, base, record))
    path.write_text(json.dumps(record, indent=2) + '\n')
    all_records = {}
    # Keep profiles isolated: two modules may deliberately define alternate
    # acceptance cases with the same name (for example old/new Q15 scope).
    for bug, modules in modules_by_bug.items():
        fingerprint = profile_fingerprint(modules)
        emit({'bug': bug, 'step': 'quality_baseline_native_started', 'revision': base})
        native, _, generated = invoke_native(worktree, 'baseline', [bug], base, args.rows, modules, args.repetitions)
        if native.returncode or profile_fingerprint(modules) != fingerprint:
            emit({'bug': bug, 'step': 'quality_baseline_harness_failed', 'exit_code': native.returncode,
                  'diagnostic_sha256': hashlib.sha256((native.stdout + native.stderr).encode()).hexdigest()})
            return None, None, 1
        all_records[bug] = [dict(item, quality_profile_sha256=fingerprint) for item in generated[bug]]
    evidence = HERE / 'verification' / ('quality-baseline-profile-' + str(time.time_ns()) + '.jsonl')
    evidence.write_text(''.join(json.dumps(record, sort_keys=True) + '\n' for group in all_records.values() for record in group))
    artifact = {'path': str(evidence.relative_to(HERE)),
                'sha256': hashlib.sha256(evidence.read_bytes()).hexdigest()}
    return all_records, artifact, None


def clear_resolved_transient_limit(case, records=()):
    limit = case.get('verification_limitation', '')
    if limit.startswith('Build watchdog stopped') and 'no native verdict' in limit:
        case.setdefault('resolved_verification_limitations', []).append({
            'limitation': limit, 'resolved_by': 'subsequent_guarded_build_and_native_verification'})
        case.pop('verification_limitation', None)
    strict = bool(records) and all(
        record.get('phase') == 'fixed' and record.get('revision') == case.get('commit')
        and record.get('binary_provenance_verified') is True
        and record.get('native_batch_guard_pass') is True
        and re.fullmatch('[0-9a-f]{64}', record.get('build_receipt_sha256', '') or '')
        and re.fullmatch('[0-9a-f]{64}', record.get('binary_provenance', {}).get('sha256', '') or '')
        and record_verdict(record) in ('functional', 'guard', 'execution_only')
        for record in records)
    sentence = ('Earlier native checks passed before exact executable receipts were required; '
                'rebuild and rerun are required for final acceptance.')
    if strict and (case.get('provenance_reverification_required') or sentence in case.get('verification_limitation', '')):
        case.pop('provenance_reverification_required', None)
        remaining = case.get('verification_limitation', '').replace(sentence, '').strip()
        if remaining:
            case['verification_limitation'] = remaining
        else:
            case.pop('verification_limitation', None)
        case.setdefault('resolved_verification_limitations', []).append({
            'limitation': sentence, 'resolved_by': 'fresh_exact_revision_receipt_bound_guarded_native_verification',
            'build_receipt_sha256': sorted({record['build_receipt_sha256'] for record in records})})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--worktree', type=Path, required=True)
    parser.add_argument('--bugs', nargs='+', required=True)
    parser.add_argument('--rows', type=int, default=24)
    parser.add_argument('--runner-only', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--build-workers', type=int)
    parser.add_argument('--generator-module', action='append', default=[])
    parser.add_argument('--quality-profile', action='append', choices=sorted(PROFILES), default=[])
    parser.add_argument('--quality-baseline-worktree', type=Path)
    parser.add_argument('--quality-ir-evidence', type=Path, action='append', default=[])
    parser.add_argument('--repetitions', type=int)
    args = parser.parse_args()
    state = json.loads((HERE / 'work-state.json').read_text())
    if not state['production_sources_frozen']:
        raise SystemExit('Source freeze is required before starting builds')
    if args.build_workers is not None and args.build_workers != state.get('authorized_build_workers'):
        raise SystemExit('Worker-count exception has not been authorized for this run')
    if args.repetitions is not None and args.repetitions < 1:
        raise SystemExit('Repetitions must be positive')
    ir_evidence = {}
    for path in args.quality_ir_evidence:
        record = json.loads(path.read_bytes())
        bug = record.get('bug')
        if bug not in ('Q01', 'Q09', 'Q11', 'Q15', 'Q20') or bug not in args.bugs or bug in ir_evidence:
            raise SystemExit('Expected one fixed runtime/IR evidence record for each requested Q01, Q09, Q11, Q15 or Q20')
        case = json.loads((HERE / 'cases' / (bug + '.json')).read_text())
        from anonymous_cpp_verify import validate_fixed_admission
        if not validate_fixed_admission(record, bug, case['commit']):
            raise SystemExit('Direct admission evidence does not match the complete fixed runtime/IR contract')
        ir_evidence[bug] = record
    modules_by_bug = {bug: generator_modules(bug, args.quality_profile, args.generator_module)
                      for bug in args.bugs if bug.startswith('Q')}
    if not args.build_only:
        missing = [bug for bug, modules in modules_by_bug.items() if not modules]
        if missing:
            raise SystemExit('Direct IR/native adapter verification is still required for: ' + ', '.join(missing))
        expected_quality_cases = {bug: selected_cases(bug, modules, args.rows)
                                  for bug, modules in modules_by_bug.items()}
        # Explicit profiles may exercise a subset, but cannot redefine the
        # finding's required scope. Bind full acceptance to the canonical
        # default profile snapshot and its exact case ownership.
        required_quality_profiles = {}
        for bug in modules_by_bug:
            required_modules = generator_modules(bug)
            required_quality_profiles[bug] = {
                'modules': required_modules,
                'owners': selected_cases(bug, required_modules, args.rows) if required_modules else {},
                'sha256': profile_fingerprint(required_modules) if required_modules else None}
    # Validate every independent branch before changing either worktree.
    for bug in args.bugs:
        case = json.loads((HERE / 'cases' / (bug + '.json')).read_text())
        parent = subprocess.check_output(['git', 'rev-parse', case['commit'] + '^'], cwd=args.worktree, text=True).strip()
        if parent != state['base']:
            raise SystemExit('Commit is not independently main-based')
    baseline, baseline_artifact, baseline_error = prepare_quality_baseline(args, state, modules_by_bug)
    if baseline_error:
        return baseline_error
    pending_quality = False
    pending_direct = False
    for bug in args.bugs:
        case_path = HERE / 'cases' / (bug + '.json')
        case = json.loads(case_path.read_text())
        revision = case['commit']
        parent = subprocess.check_output(['git', 'rev-parse', revision + '^'], cwd=args.worktree, text=True).strip()
        if parent != state['base']:
            raise SystemExit('Commit is not independently main-based')
        require_clean(args.worktree)
        subprocess.run(['git', 'switch', '--detach', revision], cwd=args.worktree, check=True, capture_output=True)
        if bug.startswith('Q') and (case['status'].startswith('partial_') or case.get('proposal_metadata', {}).get('remaining_open')):
            case['implementation_scope_complete'] = False
        case['status_before_verification'] = case['status']
        case['status'] = 'verification_in_progress'
        case_path.write_text(json.dumps(case, indent=2) + '\n')
        emit({'bug': bug, 'step': 'build_started', 'revision': revision})
        start = time.monotonic()
        target = 'ydb/tests/tools/kqprun'
        if bug == 'C41' and not args.runner_only:
            target += ' ydb/core/kqp/ut/rbo -tA'
        build, memory_metrics = run_build(args.worktree, target, args.build_workers)
        record = {'bug': bug, 'revision': revision, 'exit_code': build.returncode,
                  'elapsed_seconds': round(time.monotonic() - start, 1),
                  'target': target, 'memory': memory_metrics,
                  'diagnostic_sha256': hashlib.sha256((build.stdout + build.stderr).encode()).hexdigest()}
        (HERE / 'verification' / (bug + '-build.json')).write_text(json.dumps(record, indent=2) + '\n')
        case['build_verification'] = record
        if build.returncode:
            case['status'] = 'verification_waiting_for_memory' if memory_metrics.get('watchdog_stopped') else 'build_failed'
            case_path.write_text(json.dumps(case, indent=2) + '\n')
            emit(record)
            # This command builds only the runner; compiler messages contain no
            # generated query/fixture data. Keep ordinary test logs suppressed.
            if bug != 'C41':
                print(build.stdout + build.stderr, flush=True)
            return 1
        from binary_provenance import write_build_receipt
        receipt = write_build_receipt(args.worktree, revision, record)
        record['build_receipt'] = str(receipt)
        case['build_verification'] = record
        (HERE / 'verification' / (bug + '-build.json')).write_text(json.dumps(record, indent=2) + '\n')
        case['status'] = 'built_pending_verification'
        case_path.write_text(json.dumps(case, indent=2) + '\n')
        if args.build_only:
            emit({'bug': bug, 'step': 'build_complete', 'revision': revision, 'build_seconds': record['elapsed_seconds']})
            continue
        emit({'bug': bug, 'step': 'native_started', 'build_seconds': record['elapsed_seconds']})
        modules = modules_by_bug.get(bug, args.generator_module)
        fingerprint = profile_fingerprint(modules) if bug.startswith('Q') else None
        native, paths, generated = invoke_native(args.worktree, 'fixed', [bug], revision, args.rows, modules, args.repetitions)
        if native.returncode:
            case['status'] = 'verification_harness_failed'
            case['native_harness_exit_code'] = native.returncode
            case_path.write_text(json.dumps(case, indent=2) + '\n')
            emit({'bug': bug, 'step': 'harness_failed', 'exit_code': native.returncode,
                  'diagnostic_sha256': hashlib.sha256((native.stdout + native.stderr).encode()).hexdigest()})
            return 1
        evidence = paths[bug]
        records = generated[bug]
        if bug.startswith('Q'):
            stable_profile = profile_fingerprint(modules) == fingerprint
            records = [dict(item, quality_profile_sha256=fingerprint if stable_profile else None) for item in records]
            records, success, case['status'] = pair_quality_records(baseline.get(bug, []), records, bug, state['base'], revision,
                                                                  ir_evidence=ir_evidence.get(bug),
                                                                  required_case_owners=required_quality_profiles[bug]['owners'],
                                                                  required_profile_sha256=required_quality_profiles[bug]['sha256'])
            observed_owners = {item['case']: item.get('generator_module') for item in records}
            if observed_owners != expected_quality_cases[bug]:
                success = False
                case['status'] = 'verified_semantics_quality_pending' if all(record_verdict(item) == 'functional' for item in records) and records else 'native_failed'
                for item in records:
                    item['quality_acceptance_pass'] = False
                    item['quality_improvement_verified'] = False
                    item['quality_pairing_reasons'].append('expected_profile_case_coverage_missing')
            paired_path = HERE / 'verification' / ('quality-paired-' + bug + '-' + revision[:12] + '-' + str(time.time_ns()) + '.jsonl')
            paired_path.write_text(''.join(json.dumps(record, sort_keys=True) + '\n' for record in records))
            case['quality_verification'] = {'generator_modules': modules, 'profile_sha256': fingerprint,
                'required_generator_modules': required_quality_profiles[bug]['modules'],
                'required_profile_sha256': required_quality_profiles[bug]['sha256'],
                'required_profile_case_count': len(required_quality_profiles[bug]['owners']),
                'finding_profile_coverage_complete': bool(records) and all(
                    item.get('finding_profile_coverage_complete') is True for item in records),
                'paired_evidence': str(paired_path.relative_to(HERE)), 'baseline_revision': state['base'],
                'baseline_evidence': baseline_artifact['path'],
                'baseline_evidence_sha256': baseline_artifact['sha256'],
                'paired_checks': len(records), 'optimization_contract_passed': success}
            partial = case.get('implementation_scope_complete') is False
            if success and partial:
                case['status'] = 'partial_verified_quality'
                case['quality_verification']['finding_scope_complete'] = False
            pending_quality |= not success or partial
        else:
            success, case['status'] = summarize_verification(records, bug)
        case['native_verification'] = {'checks': len(records), 'checks_passed': success,
                                       'evidence': str(evidence.relative_to(HERE))}
        direct_pending = case['status'] == 'sql_checks_passed_direct_pending'
        pending_direct |= direct_pending
        if bug in ('C31', 'C47'):
            case['native_verification'].update(
                checks_passed=bool(records) and all(
                    record_verdict(item) in ('functional', 'guard') for item in records),
                finding_acceptance_passed=False,
                direct_local_contracts_required=True,
                direct_local_contracts_verified=False)
        if bug.startswith('Q'):
            case['native_verification']['semantic_checks_passed'] = bool(records) and all(record_verdict(item) == 'functional' for item in records)
        if success or direct_pending:
            clear_resolved_transient_limit(case, records)
        case_path.write_text(json.dumps(case, indent=2) + '\n')
        emit({'bug': bug, 'step': 'complete', 'status': case['status'], 'checks': len(records)})
        if direct_pending:
            continue
        if not success:
            for item in records:
                if record_verdict(item) == 'failure':
                    emit(item)
            if not bug.startswith('Q') or case['status'] == 'native_failed':
                return 1
    return 2 if pending_quality or pending_direct else 0


if __name__ == '__main__':
    raise SystemExit(main())
