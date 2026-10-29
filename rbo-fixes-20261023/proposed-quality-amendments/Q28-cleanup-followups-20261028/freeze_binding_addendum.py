"""Freeze exact ledger59-dependent bindings and source-only cleanup evidence."""
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import ModuleType
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROJECT = ROOT.parent
OLD_DIRECT = '0ee20f0c2c31f3e36e9002373d501a9f465c9cdcbabec1d56d64d439fec2c60d'
OLD_QUALITY = '7cda7ec6c03b18d4a78a152dfc8f53f3fcbbe5487d5df316ac351d0b8e33bc89'
OLD_WRAPPER = '6f40673cc6e1a8169185302d7895c46a40f45cb46f13dc4dad2afbb7104d60fd'
OLD_INTEGRATION_RUNNER = 'b2b939a9eee061e745b59be8307619a1fbf5e2e4a1bef619e14d0d349083f406'
LEDGER59_NATIVE = '0ddf84d7d8424a580922a63ac6d1dcaae27ff065fd44149dd88d29c294f9fc77'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def assignment(raw, name):
    node = next(node for node in ast.parse(raw).body if isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == name for target in node.targets))
    return ast.literal_eval(node.value)


def replace_one(raw, old, new):
    assert raw.count(old.encode()) == 1
    return raw.replace(old.encode(), new.encode())


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)

    patches = json.loads((HERE / 'source-images.json').read_bytes())['patches']
    mocks_path = ROOT / 'verification/Q28-cleanup-followups-metadata-checks-20261028.json'
    mocks = json.loads(mocks_path.read_bytes())
    direct_after = (HERE / 'anonymous_cpp_verify.py').read_bytes()
    depth_before = (HERE / 'q28_depth_verify.py').read_bytes()
    check(sha(direct_after) == patches[0]['after_sha256'] == mocks['sources']['anonymous_cpp_verify.py'],
          'exact_mocked_helper_after_image')
    check(sha(depth_before) == patches[1]['after_sha256'] == mocks['sources']['q28_depth_verify.py'],
          'exact_mocked_runner_cleanup_after_image')
    check(mocks['checks_passed'] == 313 and mocks['scenario_count'] == 67, 'all_cleanup_regressions_passed')
    native = (ROOT / 'native_verify.py').read_bytes()
    check(sha(native) == LEDGER59_NATIVE, 'exact_applied_ledger59_native')
    before = {name: (ROOT / name).read_bytes() for name in
              ('quality_spill_bounds.py', 'c06_c50_bounded_integration_checks.py', 'c06_c50_integration_runner.py')}
    for name, expected in (('quality_spill_bounds.py', OLD_QUALITY),
                           ('c06_c50_bounded_integration_checks.py', OLD_WRAPPER),
                           ('c06_c50_integration_runner.py', OLD_INTEGRATION_RUNNER)):
        check(sha(before[name]) == expected, 'exact_ledger59_before:' + name)
    before['q28_depth_verify.py'] = depth_before
    after = {}
    after['quality_spill_bounds.py'] = replace_one(before['quality_spill_bounds.py'], OLD_DIRECT, sha(direct_after))
    after['c06_c50_bounded_integration_checks.py'] = replace_one(before['c06_c50_bounded_integration_checks.py'],
                                                               OLD_QUALITY, sha(after['quality_spill_bounds.py']))
    integration = replace_one(before['c06_c50_integration_runner.py'], OLD_DIRECT, sha(direct_after))
    integration = replace_one(integration, OLD_QUALITY, sha(after['quality_spill_bounds.py']))
    after['c06_c50_integration_runner.py'] = replace_one(integration, OLD_WRAPPER,
                                                       sha(after['c06_c50_bounded_integration_checks.py']))
    after['q28_depth_verify.py'] = replace_one(depth_before, OLD_DIRECT, sha(direct_after))
    candidate_dir = HERE / 'binding-refresh'
    candidate_dir.mkdir(exist_ok=True)
    diff = []
    images = {}
    for name in before:
        old_tree, new_tree = ast.parse(before[name]), ast.parse(after[name])
        bind_name = '_SOURCE_BINDINGS' if name == 'quality_spill_bounds.py' else 'SOURCE_BINDINGS'
        old_bind, new_bind = assignment(before[name], bind_name), assignment(after[name], bind_name)
        check(old_bind.keys() == new_bind.keys(), 'binding_keys_unchanged:' + name)
        for tree in (old_tree, new_tree):
            tree.body = [node for node in tree.body if not (isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == bind_name for target in node.targets))]
        check(ast.dump(old_tree) == ast.dump(new_tree), 'binding_dictionary_only_change:' + name)
        (candidate_dir / name).write_bytes(after[name])
        target = ROOT.name + '/' + name
        diff.append('diff --git a/' + target + ' b/' + target + '\n'
            + ''.join(difflib.unified_diff(before[name].decode().splitlines(True), after[name].decode().splitlines(True),
                fromfile='a/' + target, tofile='b/' + target)))
        images[name] = {'before_sha256': sha(before[name]), 'after_sha256': sha(after[name]),
                       'refreshed_dependencies': {key: {'before': old_bind[key], 'after': new_bind[key]}
                                                  for key in old_bind if old_bind[key] != new_bind[key]}}
    binding_patch = ROOT / 'proposed-quality-amendments/Q28-cleanup-ledger59-binding-refresh-20261028.patch'
    binding_patch.write_text(''.join(diff))
    with tempfile.TemporaryDirectory(prefix='q28-cleanup-binding-') as temp:
        for name, raw in before.items():
            path = Path(temp) / ROOT.name / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        for command in (['git', 'apply', '--check', '--whitespace=error-all', str(binding_patch)],
                        ['git', 'apply', str(binding_patch)]):
            subprocess.run(command, cwd=temp, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for name, raw in after.items():
            check((Path(temp) / ROOT.name / name).read_bytes() == raw, 'exact_binding_patch_application:' + name)

    virtual = {(ROOT / name).resolve(): raw for name, raw in after.items()}
    virtual[(ROOT / 'anonymous_cpp_verify.py').resolve()] = direct_after
    # All pins remain exact, including the newly reachable adapter and native59.
    for name, raw in after.items():
        bindings = assignment(raw, '_SOURCE_BINDINGS' if name == 'quality_spill_bounds.py' else 'SOURCE_BINDINGS')
        for key, expected in bindings.items():
            if name == 'q28_depth_verify.py':
                path = ROOT / (key + '.py')
                if key == 'quality_scheduler_depth_checks':
                    data = (ROOT / 'proposed-quality-amendments/Q28-map-fusion-refresh-depth-20261028/quality_scheduler_depth_checks.py').read_bytes()
                else:
                    data = virtual[path.resolve()] if path.resolve() in virtual else path.read_bytes()
            else:
                path = PROJECT / key
                data = virtual[path.resolve()] if path.resolve() in virtual else path.read_bytes()
            check(sha(data) == expected, 'exact_composed_dependency:' + name + ':' + key)
    check(assignment(after['quality_spill_bounds.py'], '_FIXED_REVISIONS')['Q28']
          == '567d22ff15c8036cbc9741e0bb29daa6c140f0c1', 'production_revision_plan_remains_separate_and_pending')

    # Recompute recursive profile hashes without importing workloads or rendering.
    pairing_path = ROOT / 'quality_pairing.py'
    pairing_raw = pairing_path.read_bytes()
    tree = ast.parse(pairing_raw)
    selected = [node for node in tree.body if (isinstance(node, ast.Assign)
                 and any(isinstance(target, ast.Name) and target.id in ('PROFILES', 'AUTO') for target in node.targets))
                or (isinstance(node, ast.FunctionDef) and node.name == 'profile_fingerprint')]
    namespace = {'HERE': ROOT, 'REVIEW': PROJECT / 'rbo-review-20261021',
                 'ast': ast, 'hashlib': hashlib, 'json': json}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(pairing_path), 'exec'), namespace)
    profile_modules = {bug: list(dict.fromkeys(module for profile in profiles
                      for module in namespace['PROFILES'][profile])) for bug, profiles in namespace['AUTO'].items()}
    profile_modules['Q28_SDK_partial7'] = ['q28_compilation_measurement']
    # Explicit named profiles are separate provenance labels, not extra admitted cases.
    profile_modules.update({'named_' + label: list(modules) for label, modules in namespace['PROFILES'].items()})
    old_read_bytes = Path.read_bytes
    profiles = {}
    for label, modules in sorted(profile_modules.items()):
        old = namespace['profile_fingerprint'](modules)
        def composed_read(path):
            resolved = path.resolve()
            return virtual[resolved] if resolved in virtual else old_read_bytes(path)
        with patch.object(Path, 'read_bytes', composed_read):
            new = namespace['profile_fingerprint'](modules)
        profiles[label] = {'modules': modules, 'before_sha256': old, 'after_sha256': new, 'changed': old != new}
    check(profiles['Q28']['changed'] and profiles['Q28_SDK_partial7']['changed'], 'recursive_Q28_SQL_and_SDK_hashes_refresh')
    check(sha((ROOT / 'anonymous_cpp_verify.py').read_bytes()) == OLD_DIRECT, 'live_helper_unchanged')
    for name, raw in before.items():
        if name != 'q28_depth_verify.py':
            check((ROOT / name).read_bytes() == raw, 'live_bound_source_unchanged:' + name)

    report = {
        'schema': 'q28_cleanup_followups_source_and_binding_addendum_v1',
        'status': 'dormant_source_mock_checked_independent_review_pending',
        'production_rbo_finding': False, 'production_sources_changed': False,
        'native_build_toolchain_work_executed': False, 'generated_workload_artifacts_persisted': False,
        'patches': patches,
        'binding_refresh': {'path': str(binding_patch.relative_to(ROOT)), 'sha256': sha(binding_patch.read_bytes()),
            'requires_applied_helper_ledger': 59, 'native_sha256': LEDGER59_NATIVE, 'images': images},
        'installation_order': ['Original Q28 four-control generator and frozen fixed-runner install remain prerequisites.',
            'Apply the separate _memfd initialization repair and outer serial-release admission repair.',
            'Apply this exact ledger59 binding refresh with those repairs at an idle boundary.',
            'The independent Q28 production revision amendment and revision-binding plan remain separately required before native four-control admission.'],
        'memfd_scope': {'changed_function_only': '_memfd',
            'preserved': ['Creation flags and executable sealing permission.', 'Complete byte payload and initial rewind.',
                          'Executable mode 0700 only when requested.', 'One caller-owned descriptor on success.'],
            'repair': 'Own the descriptor across all post-create initialization and close it on BaseException; use partial-write-aware direct writes so no duplicate FD can escape fdopen ownership.',
            'limits': ['Does not repair unrelated callers that allocate several descriptors before entering cleanup.',
                       'Does not claim recovery from SIGKILL, process death, or arbitrary interruption after successful return and before caller assignment.']},
        'runner_scope': {'changed_function_only': 'main',
            'repair': 'Any outer source/serial-guard exception forces harness-failed status and local admission false, while retaining completed case/process evidence.',
            'preserved': ['Existing original26 paired evidence remains mandatory.', 'Exactly four source-bound amendment cases and 1708 expected checks.',
                          'All receipt/revision/source/descriptor/guard checks and serial execution.',
                          'SQL8 and SDK7 remain separate; all full-quality and query-semantic flags remain false.']},
        'metadata': {'checks_passed': mocks['checks_passed'], 'scenarios': mocks['scenario_count'],
            'prior_runner_checks_passed': 255, 'prior_runner_scenarios': 52,
            'report': str(mocks_path.relative_to(ROOT)), 'report_sha256': sha(mocks_path.read_bytes())},
        'source_and_binding_checks_passed': len(checks), 'source_and_binding_checks': checks,
        'recursive_profile_addendum': {'pairing_source_sha256': sha(pairing_raw), 'profiles': profiles,
            'meaning': 'New fingerprints only; no prior manifests or evidence edited and no semantic/admission success inferred.'},
        'prior_changes_requested_review': {'path': 'verification/Q28-depth-fixed-runner-independent-review-20261028.json',
            'sha256': '6c3305c6993df99da7c275e30c596d0f28fe33715697972e3b7c253d8c39c99c'},
        'untouched_original_runner': {'path': 'proposed-quality-amendments/Q28-depth-runner-20261028/q28_depth_verify.py',
            'sha256': '77b151edb0de8cfa0c73ea44f0e230513bfcc802d3d14ecaf51a512aafc06d96'},
        'unmodified_production_revision_plan': 'verification/Q28-depth-amendment-binding-update-plan-20261028.json'}
    output = ROOT / 'verification/Q28-cleanup-followups-source-and-ledger59-binding-addendum-20261028.json'
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'source_binding_checks': len(checks), 'binding_patch_sha256': sha(binding_patch.read_bytes()),
        'report_sha256': sha(output.read_bytes()), 'profile_labels': len(profiles),
        'changed_profile_labels': sum(value['changed'] for value in profiles.values())}))


if __name__ == '__main__':
    main()
