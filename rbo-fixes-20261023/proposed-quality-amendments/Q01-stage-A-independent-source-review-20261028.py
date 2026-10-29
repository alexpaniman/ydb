"""Source and metadata-only independent review; no native processes or payload files."""
from pathlib import Path
import ast
import contextlib
import hashlib
import io
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CANDIDATE = ROOT / 'proposed-quality-amendments/Q01-concrete-acceptance-20261028'
PREPARATION = ROOT / 'verification/Q01-concrete-stage-A-source-preparation-20261028.json'
PATCH = ROOT / 'proposed-quality-amendments/Q01-concrete-stage-A-acceptance-20261028.patch'
checks = []


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def check(name, condition):
    if not condition:
        raise AssertionError(name)
    checks.append(name)


def literal(raw, name):
    tree = ast.parse(raw)
    return ast.literal_eval(next(node.value for node in tree.body if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == name for target in node.targets)))


preparation = json.loads(PREPARATION.read_bytes())
check('frozen_preparation', sha(PREPARATION.read_bytes()) ==
      'b54e4ebd14f4f16d4ae31d7d9b6894f19d27fc60f3b6d7e3e5ad3558b8b8a0a9')
check('frozen_patch', sha(PATCH.read_bytes()) ==
      '46a0940c71f2281569b5e9dc01be21b3693152ea828d331c5178c8bf1f40c2b1')
proposal = ROOT / preparation['proposal']['path']
check('frozen_proposal', sha(proposal.read_bytes()) ==
      '5ebf357ad11c34658fc90b52ed6e37baa74adfe05c4689347e09e5e0e80caa66')
for name, digest in preparation['before_images'].items():
    check('actual_before:' + name, sha((ROOT / name).read_bytes()) == digest)
for name, digest in preparation['after_images'].items():
    check('candidate_after:' + name, sha((CANDIDATE / name).read_bytes()) == digest)
    ast.parse((CANDIDATE / name).read_bytes())
for group in ('production_source_bindings', 'wire_encoding_source_bindings'):
    for row in preparation[group]:
        raw = subprocess.check_output(['git', 'show', row['revision'] + ':' + row['path']], cwd=ROOT.parent)
        check('source_hash:' + row['revision'][:12] + ':' + row['path'], sha(raw) == row['sha256'])
        lines = raw.decode().splitlines()
        for anchor, locations in row['anchors'].items():
            check('source_anchor:' + row['path'] + ':' + anchor,
                  all(anchor in lines[index - 1] for index in locations))

old_spill = (ROOT / 'quality_spill_bounds.py').read_bytes()
new_spill = (CANDIDATE / 'quality_spill_bounds.py').read_bytes()
old_bindings = literal(old_spill, '_SOURCE_BINDINGS')
new_bindings = literal(new_spill, '_SOURCE_BINDINGS')
expected_changes = {ROOT.name + '/' + name for name in
                    ('quality_range_runtime_contract.py', 'quality_range_runtime_cpp_generator.py')}
check('spill_dependency_set_unchanged28', len(old_bindings) == len(new_bindings) == 28
      and set(old_bindings) == set(new_bindings))
check('spill_exact_two_hash_updates', {name for name in old_bindings
      if old_bindings[name] != new_bindings[name]} == expected_changes)
normalized = new_spill.decode()
for name in expected_changes:
    normalized = normalized.replace(new_bindings[name], old_bindings[name])
check('spill_policy_byte_identical', normalized.encode() == old_spill)
for name, digest in new_bindings.items():
    candidate = CANDIDATE / Path(name).name if name in expected_changes else ROOT.parent / name
    check('composed_spill_dependency:' + name, sha(candidate.read_bytes()) == digest)

with tempfile.TemporaryDirectory(prefix='q01-independent-patch-only-') as temporary:
    folder = Path(temporary) / ROOT.name
    folder.mkdir()
    for name in preparation['before_images']:
        (folder / name).write_bytes((ROOT / name).read_bytes())
    applied = subprocess.run(['git', 'apply', str(PATCH)], cwd=temporary, capture_output=True)
    check('clean_disposable_patch_application', applied.returncode == 0)
    for name, digest in preparation['after_images'].items():
        check('disposable_after:' + name, sha((folder / name).read_bytes()) == digest)

script = (CANDIDATE / 'verify_metadata.py').read_text()
check('frozen_metadata_script', sha(script.encode()) ==
      'daed7ab1717871e4b4ba8b7a80d08d3d847b87069c3bfece91a107c087695d9c')
# Extend the existing temporary metadata fixture in RAM. Neither its records
# nor graph examples are serialized by this independent review.
injection = '''        independent = []
        def independently_reject(values, label):
            result = validate_mock(values)
            assert result['semantic_path_pass'] is False, label
            assert result['pilot_native_identity_and_protocol_pass'] is False, label
            assert result['raw_measurement_vector_pass'] is False, label
            assert result['optimization_acceptance_pass'] is False, label
            independent.append(label)
        for ordinal in range(30):
            for field in ('q01_measurement_identity','q01_child_observation'):
                altered = list(records)
                altered[ordinal] = copy.deepcopy(records[ordinal])
                del altered[ordinal][field]
                independently_reject(altered, 'pilot_missing_'+field+'_'+str(ordinal))
            for key,value in [('run_id','f'*32),('kind','warmup'),('index',True),
                              ('executable_sha256','0'*64),('workload',{}),('cell',{})]:
                altered = list(records)
                altered[ordinal] = copy.deepcopy(records[ordinal])
                identity = altered[ordinal]['q01_measurement_identity']
                identity[key] = value
                altered[ordinal]['q01_child_observation']['bindings']['identity_sha256'] = sha(contract.encoded(identity))
                independently_reject(altered, 'pilot_coordinate_'+key+'_'+str(ordinal))
        for section,keys in [('bindings', ('protocol_source_sha256','integration_source_sha256',
                    'outer_guard_source_sha256','identity_sha256','timeout_seconds','inherited_scope_sha256')),
                             ('raw_child_report', ('exit_code','wait_status','guard_exit_code',
                    'termination_signal','core_dump_status_bit','nonce'))]:
            for key in keys:
                altered = copy.deepcopy(records[:1]) + records[1:]
                del altered[0]['q01_child_observation'][section][key]
                if section == 'raw_child_report':
                    altered[0]['q01_child_observation']['report_sha256'] = sha(contract.encoded(altered[0]['q01_child_observation'][section]))
                independently_reject(altered, 'pilot_missing_protocol_'+key)
        for key in ('local_cleanup_complete','target_group_empty','outer_memory_guard_verified'):
            altered = copy.deepcopy(records[:1]) + records[1:]
            altered[0]['q01_child_observation'][key] = False
            independently_reject(altered, 'pilot_failed_'+key)
        for section,key in [('bindings','inherited_scope_sha256'),('raw_child_report','nonce')]:
            altered = copy.deepcopy(records[:2]) + records[2:]
            altered[1]['q01_child_observation'][section][key] = altered[0]['q01_child_observation'][section][key]
            if section == 'raw_child_report':
                altered[1]['q01_child_observation']['report_sha256'] = sha(contract.encoded(altered[1]['q01_child_observation'][section]))
            independently_reject(altered, 'pilot_duplicate_'+key)
        altered = copy.deepcopy(records[:2]) + records[2:]
        for key in ('native_batch_memory_evidence','native_batch_memory_sha256'):
            altered[1][key] = altered[0][key]
        altered[1]['q01_child_observation']['outer_memory_guard_sha256'] = altered[0]['native_batch_memory_sha256']
        independently_reject(altered, 'pilot_duplicate_guard_slice')
        accepted = validate_mock(records)
        assert accepted['semantic_path_pass'] is True
        assert accepted['pilot_native_identity_and_protocol_pass'] is True
        assert accepted['raw_measurement_vector_pass'] is False
        assert accepted['resource_scaling_verified'] is False
        assert accepted['optimization_acceptance_pass'] is False
        assert accepted['canonical_SQL_652_admitted'] is False
        independent.append('complete_pilot_only_no_resource_or_quality_promotion')
        globals()['_independent_checks'] = independent
'''
marker = '        canonical_counts={}'
check('single_metadata_extension_point', script.count(marker) == 1)
environment = {'__file__': str(CANDIDATE / 'verify_metadata.py'), '__name__': 'independent_q01_metadata'}
captured = io.StringIO()
with contextlib.redirect_stdout(captured):
    exec(compile(script.replace(marker, injection + marker), str(CANDIDATE / 'verify_metadata.py'), 'exec'), environment)
    environment['main']()
author_checks = json.loads(captured.getvalue())
check('author1977_rerun', author_checks['checks_passed'] == 1977)
check('recipe1818_rerun', author_checks['existing_recipe_checks_passed'] == 1818)
checks.extend(environment['_independent_checks'])

report = {
    'schema': 'q01_stage_A_independent_source_metadata_review_v1',
    'status': 'approved_stage_A_source_only_actual_pilot_and_stage_B_pending',
    'patch': {'path': str(PATCH.relative_to(ROOT)), 'sha256': sha(PATCH.read_bytes())},
    'source_preparation': {'path': str(PREPARATION.relative_to(ROOT)), 'sha256': sha(PREPARATION.read_bytes())},
    'proposal': {'path': str(proposal.relative_to(ROOT)), 'sha256': sha(proposal.read_bytes())},
    'after_images': preparation['after_images'],
    'independent_checks_passed': len(checks),
    'checks': checks,
    'author_metadata_checks_rerun': author_checks['checks_passed'],
    'recipe_checks_rerun': author_checks['existing_recipe_checks_passed'],
    'corrected_review_finding': {
        'superseded_patch_sha256': 'c1d7c7e5de6218ea0dde8a3f5abebbb72a3a3f7016de4b108de4f34146b5b0cb',
        'bug': 'The pilot path returned before validating run/sample identity and child protocol, so stale or mislabelled pilot observations could pass a fresh manifest.',
        'resolution': 'Every pilot now requires exact pilot/index0 coordinates, current source/workload/executable identity, successful child and outer guard protocol, and unique guard path, scope identity and child nonce.'},
    'review_conclusions': [
        'The five existing-file before-images and all28 spill dependencies remain exact after unrelated C31 helper repairs. The seven-file patch applies cleanly; only two spill dependency literals change.',
        'Original16 scaling queries, fixtures and row oracles remain unchanged. Five additional suffix/fallback controls and the finite15 schedule are generator-only preparations.',
        'The Double fallback extrema use the source-confirmed integral RowsJson token encoding and retain exact ordered value/type checking.',
        'The local path observer binds stream/queue/bound/output topology, keeps unknown final peephole shapes pending, and does not establish general AST equivalence or amortized bounds.',
        'The original42 runtime specifications/checks remain required. Fifteen allocator snapshots use existing APIs and explicitly distinguish consumption from release/invalidation and historical peaks.',
        'All30 pilot records require the same existing native identity/protocol as raw samples. Successful pilot metadata does not grant resource or optimization acceptance.',
        'Raw180 observations retain one visible warmup and five alternating measured samples per phase/profile. Whole-child CPU/maxRSS includes startup/setup; Python oracle work is excluded. No ratio, setup subtraction or operator-memory claim is admitted.',
        'Canonical652, direct42, actual path30, and later measured-contract dispatch binding remain separate obligations. The existing hard-false Q01 optimization gate is unchanged.'
    ],
    'limitations': [
        'This is source/metadata approval for staged preparation only; no actual path, allocator, resource or Q01 optimization admission is granted.',
        'Native pilot may reject an unseen final peephole shape. Such rejection requires narrow source-supported review, not replacement by global callable counts.',
        'Stage B must bind its reviewed contract digest at native dispatch and enforce all required canonical/direct evidence before any optimization acceptance.',
        'No live helper or production edits, native/build/toolchain processes, or rendered SQL/C++/AST/result payload persistence occurred.'
    ]
}
destination = ROOT / 'verification/Q01-concrete-stage-A-independent-review-20261028.json'
destination.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
print(json.dumps({'path': str(destination.relative_to(ROOT)), 'sha256': sha(destination.read_bytes()),
                  'independent_checks': len(checks), 'author_checks': author_checks['checks_passed'],
                  'recipe_checks': author_checks['existing_recipe_checks_passed']}))
