"""Compose dormant helper sources and refresh only reviewed dependency hashes.

No compiler, runner, generated workload, or rendered C++ is invoked or saved.
"""
from pathlib import Path
import ast
import copy
import difflib
import hashlib
import importlib
import json
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent
PATCHES = (
    ('proposed-correctness-amendments/C31-shared-aggregate-pipeline-contracts-20261028.patch', '73a8afb3e6914ad0ab944ab5585199322c3d2175aa664ce84be1d0d50a5e8f3b'),
    ('proposed-quality-amendments/Q01-shared-window-setup-controls-20261028.patch', 'a8527c634ad50e9066bf019f8d32af1aec805e9a9ad1eebebf907631ea93a93e'),
    ('proposed-quality-amendments/Q28-sdk-compilation-pilot-20261028.patch', '9269565adb8713961c864208020b41c0f58dadb1440d347b5834a9fa4bfd8483'),
    ('proposed-quality-amendments/Q28-sdk-compilation-runtime-identity-20261028.patch', '8712ec3993b04acf34b404818f48714588628fd6418315adcaeb57a54168a9e8'),
    ('proposed-quality-amendments/Q28-sdk-compilation-version-replay-20261028.patch', '4aabcde2409f4feb9d82c7b2908e7ffec334fd29ae9677402a104e366986a49f'),
    ('proposed-quality-amendments/Q28-sdk-compilation-readonly-executable-20261028.patch', '9c782adc11186b7a4c1712a666a3b39030de2df675e6b23bb01c8ec8b8e6494a'),
    ('proposed-quality-amendments/Q-SQL-reviewed-spill-payload-cap-20261028.patch', '9e70be28e44f600870ceb0db59bf4b990b1ddc5a22cd11a6af51dfdee1952e93'),
    ('proposed-quality-amendments/Q-SQL-spill-cap-production-revision-guard-20261028.patch', 'b9393c041625cbf1b7f43d2d55aa6e7f440367a33f8f67b6ba68258423ebac54'),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(name, value):
    path = ROOT / name
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')
    return sha(path.read_bytes())


def assignment(tree, name):
    return next(n for n in tree.body if isinstance(n, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id == name for t in n.targets))


checks = 0


def check(value, label):
    global checks
    checks += 1
    if not value:
        raise AssertionError(label)


def rejected(fn, label):
    try:
        fn()
    except (ValueError, OSError, KeyError, TypeError, AttributeError):
        check(True, label)
        return
    check(False, label)


plan = json.loads((ROOT / 'verification/quality-all29-serial-execution-plan-20261028.json').read_bytes())
old_bindings = json.loads((ROOT / 'verification/Q-SQL-spill-cap-preparation-bindings-20261028.json').read_bytes())['source_bindings']
live_sources = {}
for folder in (ROOT, REPO / 'rbo-review-20261021'):
    for path in folder.iterdir():
        if path.is_file() and path.suffix in ('.py', '.json', '.conf'):
            live_sources[str(path.relative_to(REPO))] = path.read_bytes()

with tempfile.TemporaryDirectory(prefix='quality-idle-composition-source-') as temporary:
    temp = Path(temporary).resolve()
    for name, raw in live_sources.items():
        destination = temp / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
    shutil.copytree(ROOT / 'cases', temp / ROOT.name / 'cases')
    stack = []
    touched = set()
    for name, digest in PATCHES:
        patch = ROOT / name
        check(sha(patch.read_bytes()) == digest, 'frozen patch identity')
        files = [line[6:] for line in patch.read_text().splitlines() if line.startswith('+++ b/')]
        before = {name: sha((temp / name).read_bytes()) if (temp / name).exists() else None for name in files}
        applied = subprocess.run(['git', 'apply', str(patch)], cwd=temp, capture_output=True)
        check(applied.returncode == 0, 'ordered temporary patch application')
        after = {name: sha((temp / name).read_bytes()) for name in files}
        stack.append(dict(path=name, sha256=digest, before_images=before, after_images=after,
                          apply_rule='Apply only when every before-image matches, including absent new files.',
                          already_applied_rule='Skip only when every after-image matches at this exact step; reject mixed or unknown images.'))
        touched.update(files)

    helper_path = temp / ROOT.name / 'quality_spill_bounds.py'
    original_helper = helper_path.read_text()
    original_tree = ast.parse(original_helper)
    original_assignment = assignment(original_tree, '_SOURCE_BINDINGS')
    check(ast.literal_eval(original_assignment.value) == old_bindings, 'original 28 bindings')
    new_bindings = {name: sha((temp / name).read_bytes()) for name in old_bindings}
    changes = {name: dict(before=old_bindings[name], after=digest)
               for name, digest in new_bindings.items() if digest != old_bindings[name]}
    check(set(changes) == {ROOT.name + '/anonymous_cpp_verify.py', ROOT.name + '/correctness_cpp_contract.py'},
          'only reviewed composed helper dependencies changed')
    refreshed = original_helper
    for name, values in changes.items():
        check(refreshed.count(values['before']) == 1, 'unique dependency hash literal')
        refreshed = refreshed.replace(values['before'], values['after'])
    refreshed_tree = ast.parse(refreshed)
    refreshed_assignment = assignment(refreshed_tree, '_SOURCE_BINDINGS')
    check(ast.literal_eval(refreshed_assignment.value) == new_bindings, 'all 28 refreshed bindings')
    original_tree.body.remove(original_assignment)
    refreshed_tree.body.remove(refreshed_assignment)
    check(ast.dump(original_tree) == ast.dump(refreshed_tree), 'binding-only AST change')
    helper_path.write_text(refreshed)
    patch_name = 'proposed-quality-amendments/Q-SQL-spill-cap-composed-dependency-refresh-20261028.patch'
    refresh_patch = ''.join(difflib.unified_diff(original_helper.splitlines(True), refreshed.splitlines(True),
        fromfile='a/' + ROOT.name + '/quality_spill_bounds.py', tofile='b/' + ROOT.name + '/quality_spill_bounds.py'))
    (ROOT / patch_name).write_text(refresh_patch)
    candidate_name = 'proposed-quality-amendments/quality_spill_bounds_composed_20261028.candidate.py'
    (ROOT / candidate_name).write_text(refreshed)
    stack.append(dict(path=patch_name, sha256=sha(refresh_patch.encode()),
                      before_images={ROOT.name + '/quality_spill_bounds.py': sha(original_helper.encode())},
                      after_images={ROOT.name + '/quality_spill_bounds.py': sha(refreshed.encode())},
                      apply_rule='Apply only when the exact guarded helper before-image matches.',
                      already_applied_rule='Skip only when the exact final guarded helper after-image matches.'))

    sys.path[:0] = [str(temp / ROOT.name), str(temp / 'rbo-review-20261021')]
    import native_verify as native
    import quality_spill_bounds as helper
    import quality_pairing as pairing
    check(Path(native.__file__).resolve() == temp / ROOT.name / 'native_verify.py', 'actual composed native module')
    check(helper._SOURCE_BINDINGS == new_bindings, 'actual composed dependency set')
    receipt_path = temp / 'synthetic-receipt.json'
    runner = temp / 'synthetic-worktree/ydb/tests/tools/kqprun/kqprun'

    def token(bug, phase):
        revision = helper._BASE_REVISION if phase == 'baseline' else helper._FIXED_REVISIONS[bug]
        binary = dict(resolved_path=str(runner), sha256='a' * 64, size_bytes=1,
                      mtime_ns=1, ctime_ns=1, inode=1, device=1)
        receipt = dict(schema_version='successful_runner_build_v1', worktree=str(runner.parents[4]),
                       revision=revision, build=dict(exit_code=0, target='ydb/tests/tools/kqprun'), binary=binary)
        raw = json.dumps(receipt, sort_keys=True).encode()
        receipt_path.write_bytes(raw)
        provenance = dict(binary_provenance_version='successful_runner_build_v1', binary_provenance_verified=True,
                          binary_provenance=binary, build_receipt_path=str(receipt_path), build_receipt_sha256=sha(raw))
        return helper.validated_quality_spill_scope([bug], phase, revision, provenance)

    adapted = unchanged = cells = 0
    vectors = {}
    modules = {}
    for finding in plan['findings']:
        bug = finding['bug']
        for profile in finding['sql']:
            name = profile['module']
            module = modules.setdefault(name, native.import_generator_snapshot(name))
            names = [case for case in module.recipes(24) if case.startswith(bug.lower() + '_')]
            vector = []
            for case in names:
                metadata = native.module_metadata(module, case, 'new', 24)
                vector.append(dict(case=case, mode='new', rows=24,
                    repetitions=metadata.get('repetitions', 1), nodes=metadata.get('node_count', 1),
                    query_syntax=metadata.get('query_syntax', 'yql'),
                    additional_ir_admission_required=metadata.get('additional_ir_admission_required', False)))
                cells += 1
                if case in helper._CELLS:
                    original = copy.deepcopy(metadata)
                    for phase in ('baseline', 'fixed'):
                        scope = token(bug, phase)
                        bounded = helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata, scope=scope, runner=runner)
                        expected = copy.deepcopy(metadata)
                        expected['table_service_overrides'].update(EnableSpillingNodes='None', SpillingServiceConfig=helper.LOCAL_SERVICE)
                        policy = bounded.pop('spill_payload_file_policy')
                        check(bounded == expected, 'only two settings change')
                        check(policy['cap_rejection_counts_as_success'] is False and policy['runtime_spill_occurrence_observed'] is False,
                              'no native admission or spill claim')
                        check(policy['source_bindings_sha256'] == helper._digest(new_bindings), 'composed source binding in policy')
                        rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata), 'missing revision token')
                        rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                                  scope=scope._replace(revision='f' * 40), runner=runner), 'unknown revision')
                    check(metadata == original, 'original metadata unchanged')
                    adapted += 1
                else:
                    check(helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata) is metadata,
                          'unaffected metadata object preserved')
                    unchanged += 1
            digest = sha(json.dumps(vector, sort_keys=True, separators=(',', ':')).encode())
            check(digest == profile['vector_sha256'], 'exact existing SQL vector')
            vectors[bug + ':' + name] = dict(case_count=len(vector), vector_sha256=digest)
    check((cells, adapted, unchanged) == (850, 82, 768), 'complete unchanged matrix')

    first = next(iter(helper._CELLS))
    entry = helper._CELLS[first]
    module = modules[entry['module']]
    metadata = native.module_metadata(module, first, 'new', 24)
    scope = token(first.split('_', 1)[0].upper(), 'fixed')
    for name in helper._SOURCE_BINDINGS:
        path = temp / name
        raw = path.read_bytes()
        path.write_bytes(raw + b'\n')
        try:
            rejected(lambda: helper.bound_quality_spill_metadata(module, first, 'new', 24, metadata,
                      scope=scope, runner=runner), 'any changed dependency rejected')
        finally:
            path.write_bytes(raw)
    for name, values in changes.items():
        original_digest = helper._SOURCE_BINDINGS[name]
        helper._SOURCE_BINDINGS[name] = values['before']
        try:
            rejected(lambda: helper.bound_quality_spill_metadata(module, first, 'new', 24, metadata,
                      scope=scope, runner=runner), 'stale pre-composition binding rejected')
        finally:
            helper._SOURCE_BINDINGS[name] = original_digest

    after_images = {name: sha((temp / name).read_bytes()) for name in sorted(touched)}
    for name in touched:
        if name.endswith('.py'):
            ast.parse((temp / name).read_bytes())
            check(True, 'composed Python syntax')
        elif name.endswith('.json'):
            json.loads((temp / name).read_bytes())
            check(True, 'composed JSON syntax')

    # Reuse the real fingerprint algorithm at canonical installed paths. The
    # temporary files contain the proposed after-images; paths must not bind a
    # future receipt to this disposable directory.
    def canonical_fingerprint(names):
        todo = [temp / ROOT.name / (name + '.py') for name in names]
        todo += [temp / ROOT.name / name for name in ('native_verify.py', 'verification_contract.py', 'quality_pairing.py')]
        found = {}
        while todo:
            path = todo.pop().resolve()
            if path in found:
                continue
            raw = path.read_bytes()
            found[path] = sha(raw)
            for node in ast.walk(ast.parse(raw)):
                imported = ([a.name for a in node.names] if isinstance(node, ast.Import)
                            else [node.module] if isinstance(node, ast.ImportFrom) and node.module else [])
                for name in imported:
                    for folder in (temp / ROOT.name, temp / 'rbo-review-20261021'):
                        candidate = folder / (name.split('.')[0] + '.py')
                        if candidate.is_file():
                            todo.append(candidate)
        for name in ('new.conf', 'legacy.conf'):
            path = temp / 'rbo-review-20261021' / name
            found[path] = sha(path.read_bytes())
        files = {str(REPO / path.relative_to(temp)): digest for path, digest in found.items()}
        temp_files = {str(path): digest for path, digest in found.items()}
        check(sha(json.dumps(temp_files, sort_keys=True).encode()) == pairing.profile_fingerprint(names),
              'real profile fingerprint algorithm agrees')
        return dict(sha256=sha(json.dumps(files, sort_keys=True).encode()), files=files)

    fingerprints = {}
    for finding in plan['findings']:
        names = finding['default_modules'] + finding['extra_modules']
        if names:
            fingerprints[finding['bug']] = canonical_fingerprint(names)
    fingerprints['Q01_resource_pilot'] = canonical_fingerprint(['quality_q01_scaling_checks'])
    fingerprints['Q28_SDK_pilot'] = canonical_fingerprint(['q28_compilation_measurement', 'quality_scheduler_semantic_checks'])
    for name, raw in live_sources.items():
        check((REPO / name).read_bytes() == raw, 'live sources unchanged')

    c31_review = 'verification/C31-shared-aggregate-pipeline-independent-review-20261028.json'
    c31_review_sha256 = '3a278030556f0f429c3577a2b924d800ceac298ae469efd1b820e3107c1c0803'
    check(sha((ROOT / c31_review).read_bytes()) == c31_review_sha256, 'C31 independent frozen predecessor review')
    report = dict(schema_version='quality_idle_helper_composition_v1', status='source_prepared_pending_binding_refresh_independent_review',
                  ordered_patches=stack, base_applied_helper_count=44, proposed_applied_helper_count=53,
                  live_application_performed=False, C31_independent_review=dict(path=c31_review, sha256=c31_review_sha256),
                  idle_checkpoints=dict(after_C31_step=dict(applied_helper_count=45,
                      exact_images=stack[0]['after_images'], remaining_start_index=1,
                      allowed_pause='Root may run C31 direct37 here, then revalidate these source hashes at the next idle boundary before the remaining eight patches.'),
                      final=dict(applied_helper_count=53, exact_images=after_images)),
                  before_images={name: sha(live_sources[name]) if name in live_sources else None for name in sorted(touched)},
                  after_images=after_images, dependency_refresh=dict(patch=patch_name, candidate=candidate_name,
                      dependency_count=28, changed_dependencies=changes, all_dependencies=new_bindings, binding_only_AST=True),
                  verification=dict(checks_passed=checks, SQL_cells=cells, adapted_cells=adapted, unchanged_cells=unchanged,
                      case_phase_pairs=164, changed_dependency_negatives=28, stale_binding_negatives=2,
                      vectors=vectors, live_edits=False, native_build_toolchain_graph_runs=0,
                      generated_SQL_CPP_fixture_or_plan_payloads_persisted=False),
                  quality_full_admission=False, ordinary_semantic_and_quality_gates_unchanged=True,
                  limits=['C31 exact predecessor is source-approved; the binding-only refresh still requires independent review.',
                          'Q01 resource and Q28 timing/full-quality acceptance remain false.',
                          'The cap prevents reviewed payload writes; an attempted spill rejected by it remains a failed query.',
                          'Apply only at the root-owned idle source boundary; actual native outcomes remain pending.'])
    report_name = 'verification/quality-idle53-source-composition-20261028.json'
    report_sha = write_json(report_name, report)
    fingerprints_sha = write_json('verification/quality-idle53-profile-fingerprints-20261028.json',
        dict(schema_version='quality_dormant_profile_after_images_v1', composition_sha256=report_sha,
             canonical_root=str(REPO), fingerprints=fingerprints, native_admission=False))
    print(json.dumps(dict(composition=report_name, sha256=report_sha, refresh_patch_sha256=sha(refresh_patch.encode()),
                          profile_fingerprints_sha256=fingerprints_sha, checks=checks,
                          after_images=after_images), sort_keys=True))
