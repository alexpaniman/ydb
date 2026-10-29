"""Compose reviewed helpers in a disposable tree; no live edits or native work."""
import ast
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from types import ModuleType
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROJECT = ROOT.parent
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
OLD_Q28 = '567d22ff15c8036cbc9741e0bb29daa6c140f0c1'
NEW_Q28 = '8089888107b179ca5c08f145f70564bd4637daa6'
HEADER = 'ydb/core/kqp/opt/rbo/kqp_rbo_rules.h'
NATIVE59 = '0ddf84d7d8424a580922a63ac6d1dcaae27ff065fd44149dd88d29c294f9fc77'
PATCHES = [
    ('Q01-concrete-stage-A-acceptance-20261028.patch', '46a0940c71f2281569b5e9dc01be21b3693152ea828d331c5178c8bf1f40c2b1', ''),
    ('Q14-bounded-pair-configuration-20261028.patch', '7969cd26b2f1caa3a036b34f9ed650a3e701624ad2e1dcea25741599f724670e', ROOT.name),
    ('Q28-map-fusion-refresh-depth-controls-20261028.patch', '05c568415beb0c5e0155e1dc1067135cdb5f2e539609f219585ef86c2ca129f7', ''),
    ('Q28-depth-fixed-runner-20261028.patch', '1985cf94a04fa93bbf97e7e2eae414ea086d8cbbb71bd14d8d053febc452f178', ''),
    ('Q28-anonymous-memfd-initialization-cleanup-20261028.patch', '758296f19b13896c8421e31420879168dc1b93b030646183cee4c533803bb10d', ''),
    ('Q28-depth-serial-release-admission-20261028.patch', 'bb9a860ae89d5ff646ea9e05e4d8c067665f0f475fceaf1dd7740db91204072d', ''),
    ('Q28-cleanup-ledger59-binding-refresh-20261028.patch', '5edec2db83095cc2f39860978ea2d4e6939ef1511c902c275b6aa25bb84f1193', ''),
]


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def git(*args):
    return subprocess.check_output(['git', '-C', str(PROJECT), *args], stderr=subprocess.PIPE)


def assignment(raw, name):
    node = next(node for node in ast.parse(raw).body if isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == name for target in node.targets))
    return ast.literal_eval(node.value)


def replace_one(raw, before, after):
    assert raw.count(before.encode()) == 1
    return raw.replace(before.encode(), after.encode())


def functions(raw):
    return {node.name: ast.dump(node) for node in ast.parse(raw).body if isinstance(node, ast.FunctionDef)}


def paths_from_patch(raw, directory):
    return [str(Path(directory) / line[6:]) for line in raw.decode().splitlines() if line.startswith('+++ b/')]


def main():
    checks, manifest = [], []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)

    check(sha((ROOT / 'native_verify.py').read_bytes()) == NATIVE59, 'exact_live_native59')
    check(git('show', '-s', '--format=%P', NEW_Q28).decode().strip() == BASE, 'new_Q28_is_independent_main_child')
    check(git('diff', '--name-only', OLD_Q28, NEW_Q28).decode().splitlines() == [HEADER],
          'new_Q28_diff_has_exact_one_production_path')
    header = git('show', NEW_Q28 + ':' + HEADER)
    check(sha(header) == '73e36b5ccdbf430b4e5e7feaddc8cc7cd14547c44d6e3fbbb32d80ae4896d27c', 'exact_reviewed_Q28_header')
    old_header = git('show', OLD_Q28 + ':' + HEADER)
    prefix, into_map = old_header.split(b'class TPushMapElementsIntoMapRule', 1)
    expected_header = prefix + b'class TPushMapElementsIntoMapRule' + into_map.replace(
        b'LocalRewriteDepth() const override { return 2; }', b'LocalRewriteDepth() const override { return 1; }', 1)
    check(header == expected_header,
          'exact_reviewed_one_line_depth_amendment')
    spill_proof_path = ROOT / 'verification/Q-SQL-spill-cap-production-source-bindings-20261028.json'
    spill_proof_raw = spill_proof_path.read_bytes()
    spill_proof = json.loads(spill_proof_raw)
    source_hashes = {}
    for path, expected in spill_proof['base_source_sha256'].items():
        actual = sha(git('show', NEW_Q28 + ':' + path))
        check(actual == expected, 'new_Q28_audited_spill_source:' + path)
        source_hashes[path] = actual
    check(len(source_hashes) == 48 and HEADER not in source_hashes, 'unchanged_48_source_spill_scope')
    case_raw = (ROOT / 'cases/Q28.json').read_bytes()
    case = json.loads(case_raw)
    series_raw = (ROOT / 'manifest.json').read_bytes()
    series = json.loads(series_raw)
    item = next(value for value in series['items'] if value['id'] == 'Q28')
    check(case['commit'] == item['commit'] == NEW_Q28 and case['status'] == item['status'] == 'prepared_unverified',
          'root_already_bound_case_and_manifest_without_native_admission')

    patch_sources, targets = [], set()
    for name, expected, directory in PATCHES:
        path = ROOT / 'proposed-quality-amendments' / name
        raw = path.read_bytes()
        check(sha(raw) == expected, 'exact_reviewed_patch:' + name)
        files = paths_from_patch(raw, directory)
        check(files and len(files) == len(set(files)), 'unique_patch_targets:' + name)
        targets.update(files)
        patch_sources.append((path, raw, directory, files))
    before = {path: (PROJECT / path).read_bytes() if (PROJECT / path).exists() else None for path in sorted(targets)}
    before['rbo-fixes-20261023/native_verify.py'] = (ROOT / 'native_verify.py').read_bytes()
    with tempfile.TemporaryDirectory(prefix='quality-idle60-compose-') as temp:
        tree = Path(temp)
        for path, raw in before.items():
            if raw is not None:
                target = tree / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(raw)
        for ordinal, (path, raw, directory, files) in enumerate(patch_sources, 1):
            snapshots = {name: sha((tree / name).read_bytes()) if (tree / name).exists() else None for name in files}
            for command in (['git', 'apply', '--check', '--whitespace=error-all', str(path)], ['git', 'apply', str(path)]):
                subprocess.run(command, cwd=tree / directory, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            manifest.append({'ordinal': ordinal, 'path': str(path.relative_to(ROOT)), 'sha256': sha(raw),
                             'working_directory_relative_to_project': directory or '.', 'before_images': snapshots,
                             'after_images': {name: sha((tree / name).read_bytes()) for name in files}})
        intermediate = {path: (tree / path).read_bytes() for path in sorted(before)}
        final = dict(intermediate)
        cap_path = ROOT.name + '/quality_spill_bounds.py'
        wrapper_path = ROOT.name + '/c06_c50_bounded_integration_checks.py'
        integration_path = ROOT.name + '/c06_c50_integration_runner.py'
        cap = replace_one(intermediate[cap_path], OLD_Q28, NEW_Q28)
        final[cap_path] = cap
        cap_hash = sha(cap)
        wrapper_binding = assignment(intermediate[wrapper_path], 'SOURCE_BINDINGS')
        final[wrapper_path] = replace_one(intermediate[wrapper_path], wrapper_binding[cap_path], cap_hash)
        runner_binding = assignment(intermediate[integration_path], 'SOURCE_BINDINGS')
        runner = replace_one(intermediate[integration_path], runner_binding[cap_path], cap_hash)
        final[integration_path] = replace_one(runner, runner_binding[wrapper_path], sha(final[wrapper_path]))
        refresh_files = [cap_path, wrapper_path, integration_path]
        diff = []
        for path in refresh_files:
            check(functions(intermediate[path]) == functions(final[path]), 'final_refresh_preserves_all_functions:' + path)
            diff.append('diff --git a/' + path + ' b/' + path + '\n'
                + ''.join(difflib.unified_diff(intermediate[path].decode().splitlines(True), final[path].decode().splitlines(True),
                    fromfile='a/' + path, tofile='b/' + path)))
        refresh = ROOT / 'proposed-quality-amendments/quality-idle60-final-revision-binding-refresh-20261028.patch'
        refresh.write_text(''.join(diff))
        for command in (['git', 'apply', '--check', '--whitespace=error-all', str(refresh)], ['git', 'apply', str(refresh)]):
            subprocess.run(command, cwd=tree, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(all((tree / path).read_bytes() == raw for path, raw in final.items()), 'exact_complete_disposable_stack')
        manifest.append({'ordinal': len(manifest) + 1, 'path': str(refresh.relative_to(ROOT)), 'sha256': sha(refresh.read_bytes()),
                         'working_directory_relative_to_project': '.',
                         'before_images': {name: sha(intermediate[name]) for name in refresh_files},
                         'after_images': {name: sha(final[name]) for name in refresh_files}})

    final_dir = HERE / 'after-images'
    final_dir.mkdir(exist_ok=True)
    for path, raw in final.items():
        (final_dir / Path(path).name).write_bytes(raw)
    virtual = {(PROJECT / path).resolve(): raw for path, raw in final.items()}
    def bytes_for(path):
        return virtual[path.resolve()] if path.resolve() in virtual else path.read_bytes()

    check(final[ROOT.name + '/native_verify.py'] == before[ROOT.name + '/native_verify.py'],
          'native59_Q01_dispatch_and_integration_empty_witness_byte_identical')
    # Exact source dependencies, including new modules, are checked after every literal reconciliation.
    for name in ('quality_spill_bounds.py', 'c06_c50_bounded_integration_checks.py', 'c06_c50_integration_runner.py', 'q28_depth_verify.py'):
        data = final[ROOT.name + '/' + name]
        key = '_SOURCE_BINDINGS' if name == 'quality_spill_bounds.py' else 'SOURCE_BINDINGS'
        pins = assignment(data, key)
        for dependency, expected in pins.items():
            target = ROOT / (dependency + '.py') if name == 'q28_depth_verify.py' else PROJECT / dependency
            check(sha(bytes_for(target)) == expected, 'exact_final_dependency:' + name + ':' + dependency)
    old_cap = before[cap_path]
    check(assignment(old_cap, '_CELLS') == assignment(cap, '_CELLS'), 'all82_spill_cells_byte_equivalent')
    check(len(assignment(cap, '_CELLS')) == 82, 'exact82_spill_cells')
    check(functions(old_cap) == functions(cap), 'spill_policy_functions_unchanged')
    old_revisions, new_revisions = assignment(old_cap, '_FIXED_REVISIONS'), assignment(cap, '_FIXED_REVISIONS')
    check({key for key in old_revisions if old_revisions[key] != new_revisions[key]} == {'Q28'}, 'only_Q28_fixed_revision_changed')
    check(new_revisions['Q28'] == NEW_Q28, 'new_Q28_revision_token_exact')
    for key in ('LOCAL_SERVICE', 'VERSION', '_BASE_REVISION'):
        check(assignment(old_cap, key) == assignment(cap, key), 'unchanged_spill_constant:' + key)
    # All non-binding component behavior must equal its independently reviewed after-image.
    component_reports = [
        ROOT / 'verification/Q01-concrete-stage-A-source-preparation-20261028.json',
        ROOT / 'verification/Q14-bounded-pair-configuration-source-preparation-20261028.json']
    for report_path in component_reports:
        report = json.loads(report_path.read_bytes())
        for name, expected in report['after_images'].items():
            if name != 'quality_spill_bounds.py':
                check(sha(final[ROOT.name + '/' + name]) == expected, 'exact_reviewed_component_after:' + name)
    cleanup_report = json.loads((ROOT / 'verification/Q28-cleanup-followups-source-and-ledger59-binding-addendum-20261028.json').read_bytes())
    for name in ('anonymous_cpp_verify.py', 'q28_depth_verify.py'):
        expected = (cleanup_report['patches'][0]['after_sha256'] if name == 'anonymous_cpp_verify.py'
                    else cleanup_report['binding_refresh']['images'][name]['after_sha256'])
        check(sha(final[ROOT.name + '/' + name]) == expected, 'exact_reviewed_cleanup_after:' + name)
    q28_driver = final[ROOT.name + '/q28_depth_verify.py']
    for source, expected in assignment(q28_driver, 'PRODUCTION_BINDINGS').items():
        check(sha(git('show', NEW_Q28 + ':' + source)) == expected, 'Q28_extra4_actual_production_binding:' + source)
    check(assignment(q28_driver, 'EXPECTED_CHECKS') == (136, 472, 238, 862), 'extra4_exact_assertion_vector1708')

    # Compute canonical-path profiles over virtual final bytes, without importing generators.
    pairing_path = ROOT / 'quality_pairing.py'
    parsed = ast.parse(pairing_path.read_bytes())
    selected = [node for node in parsed.body if (isinstance(node, ast.Assign)
                 and any(isinstance(target, ast.Name) and target.id in ('PROFILES', 'AUTO') for target in node.targets))
                or (isinstance(node, ast.FunctionDef) and node.name == 'profile_fingerprint')]
    scope = {'HERE': ROOT, 'REVIEW': PROJECT / 'rbo-review-20261021', 'ast': ast, 'json': json, 'hashlib': hashlib}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(pairing_path), 'exec'), scope)
    profiles = {bug: list(dict.fromkeys(module for profile in labels for module in scope['PROFILES'][profile]))
                for bug, labels in scope['AUTO'].items()}
    profiles.update({'Q01_resource_pilot': ['quality_q01_scaling_checks'],
                     'Q01_concrete_stage_A': ['quality_q01_scaling_checks', 'quality_q01_concrete_contract'],
                     'Q14': ['quality_join_shuffle_checks'],
                     'Q25': ['quality_partition_checks'],
                     'Q28_SDK_pilot': ['q28_compilation_measurement', 'quality_scheduler_semantic_checks']})
    old_read, old_isfile = Path.read_bytes, Path.is_file
    def composed_read(path):
        return virtual[path.resolve()] if path.resolve() in virtual else old_read(path)
    def composed_isfile(path):
        return path.resolve() in virtual or old_isfile(path)
    profile_rows = {}
    for label, modules in sorted(profiles.items()):
        old = None if label == 'Q01_concrete_stage_A' else scope['profile_fingerprint'](modules)
        with patch.object(Path, 'read_bytes', composed_read), patch.object(Path, 'is_file', composed_isfile):
            new = scope['profile_fingerprint'](modules)
        profile_rows[label] = {'modules': modules, 'ledger59_before_sha256': old, 'after_sha256': new}

    untouched_sources = ['quality_scheduler_cpp_checks.py', 'quality_scheduler_semantic_checks.py',
        'q28_compilation_measurement.py', 'quality_rule_cpp_generator.py', 'quality_window_checks.py',
        'quality_sum_checks.py', 'quality_minmax_checks.py', 'quality_range_minmax_checks.py',
        'quality_pairing.py', 'q01_native_child_measurement.py', 'anonymous_cpp_guard.py',
        'anonymous_cpp_metadata.py', 'production_version_object.py', 'correctness_cpp_contract.py']
    untouched = {name: sha((ROOT / name).read_bytes()) for name in untouched_sources}
    check(untouched['quality_scheduler_cpp_checks.py'] == 'b8d6342b555c2b2b69d0f5130112f3630cf5d6f2607b357831bc95645d30dcb7',
          'original_Q28_direct26_generator_unchanged')
    check(untouched['q28_compilation_measurement.py'] == 'bacb3f2b89a203ab3715a5e87e9dc1ca50d70d2baf7e5823c6692a1cc73b7e3c',
          'SDK_partial7_adapter_unchanged')
    for path, raw in before.items():
        current = PROJECT / path
        check((not current.exists()) if raw is None else current.read_bytes() == raw, 'live_source_unchanged:' + path)

    report = {
        'schema': 'quality_idle60_ordered_source_composition_v1',
        'status': 'dormant_source_checked_independent_review_and_native_pending',
        'required_live_helper_ledger': 59, 'native59_sha256': NATIVE59,
        'ordered_patches': manifest,
        'before_images': {path: sha(raw) if raw is not None else None for path, raw in before.items()},
        'after_images': {path: sha(raw) for path, raw in final.items()},
        'after_images_directory': str(final_dir.relative_to(ROOT)),
        'exact_function_behavior': 'Each functional component equals its reviewed after-image; final reconciliation changes only existing dependency hashes and the Q28 revision literal.',
        'native_merge': 'No Q01 Stage A native hunk exists. Installed ledger59 Q01 child measurement and integration empty-result dispatch are preserved byte-for-byte.',
        'new_Q28_revision': {'revision': NEW_Q28, 'parent': BASE, 'old_revision': OLD_Q28,
            'only_changed_path_from_old': HEADER, 'header_sha256': sha(header),
            'case_sha256': sha(case_raw), 'manifest_sha256_at_preparation': sha(series_raw),
            'case_and_manifest_changed_by_this_composition': False,
            'status': 'prepared_unverified', 'audited_spill_sources': source_hashes,
            'spill_source_proof_path': str(spill_proof_path.relative_to(ROOT)), 'spill_source_proof_sha256': sha(spill_proof_raw)},
        'profiles': profile_rows, 'untouched_source_bindings': untouched,
        'coverage_and_admission': {
            'Q01': {'runtime': 42, 'canonical_SQL': 652, 'path_pilot': 30, 'raw_resource': 180,
                'original42_specifications_and_existing_native_checks_preserved': True,
                'new_allocator_fields_require_fresh_runtime_evidence': True, 'StageA_optimization_admission': False},
            'Q14': {'direct': 12, 'SQL_pair_cases': 6, 'SQL_nodes': 4, 'full_quality_admission_from_source': False},
            'Q28': {'required_original_paired_direct': 26, 'separate_fixed_amendment': 4,
                'amendment_expected_checks': [136,472,238,862], 'amendment_expected_checks_total': 1708,
                'canonical_SQL': 8, 'SDK_pilot_covered': 7, 'SDK_shared_spilling_pending': 1,
                'SDK_pure_optimizer_timing': False, 'full_quality_admission_from_source': False},
            'integration': {'strict_records': 16, 'new_empty_result_witness_and_original_failures_preserved': True},
            'spill': {'cells': 82, 'nodes_and_payload_cap_policy_unchanged': True},
            'preserved_guards': ['Existing serial slots, memory limits and timeout/cleanup requirements.',
                'Fresh build receipt, exact production/source/generator/executable identity and case vectors.',
                'Strict failure retention and no source-only acceptance promotion.']},
        'source_checks_passed': len(checks), 'source_checks': checks,
        'generated_SQL_CPP_fixtures_rows_plans_AST_persisted': False,
        'native_build_toolchain_or_Git_mutation_performed': False, 'live_helper_edits': False,
        'historical_reports_modified': False}
    target = ROOT / 'verification/quality-idle60-ordered-source-composition-20261028.json'
    target.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'source_checks_passed': len(checks), 'ordered_patches': len(manifest),
        'after_images': len(final), 'profile_labels': len(profile_rows), 'report_sha256': sha(target.read_bytes()),
        'final_refresh_patch_sha256': sha(refresh.read_bytes())}))


if __name__ == '__main__':
    main()
