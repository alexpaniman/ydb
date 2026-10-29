"""Metadata-only revision-scope contracts; never launch a runner or render inputs."""
from pathlib import Path
import ast
import copy
import hashlib
import json
import subprocess
import sys
import tempfile
import types

ROOT = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(ROOT), str(ROOT.parent / 'rbo-review-20261021')]
import native_verify as live

bindings = json.loads((ROOT / 'verification/Q-SQL-spill-revision-preparation-bindings-20261028.json').read_bytes())
original = json.loads((ROOT / 'verification/Q-SQL-spill-cap-preparation-bindings-20261028.json').read_bytes())
helper = types.ModuleType('quality_spill_bounds')
helper.__file__ = str(ROOT / 'quality_spill_bounds.py')
sys.modules[helper.__name__] = helper
exec(compile((ROOT / bindings['helper_candidate']).read_bytes(), helper.__file__, 'exec'), helper.__dict__)
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


with tempfile.TemporaryDirectory(prefix='quality-spill-token-metadata-') as temporary:
    temporary = Path(temporary).resolve()
    receipt_path = temporary / 'receipt.json'
    worktree = temporary / 'worktree'
    runner = worktree / 'ydb/tests/tools/kqprun/kqprun'

    def provenance(revision):
        binary = dict(resolved_path=str(runner), sha256='a' * 64, size_bytes=1,
                      mtime_ns=1, ctime_ns=1, inode=1, device=1)
        receipt = dict(schema_version='successful_runner_build_v1', worktree=str(worktree),
                       revision=revision, build=dict(exit_code=0, target='ydb/tests/tools/kqprun'),
                       binary=binary)
        raw = json.dumps(receipt, sort_keys=True).encode()
        receipt_path.write_bytes(raw)
        return dict(binary_provenance_version='successful_runner_build_v1',
                    binary_provenance_verified=True, binary_provenance=copy.deepcopy(binary),
                    build_receipt_path=str(receipt_path), build_receipt_sha256=hashlib.sha256(raw).hexdigest())

    for case, entry in helper._CELLS.items():
        bug = case.split('_', 1)[0].upper()
        module = live.import_generator_snapshot(entry['module'])
        metadata = module.metadata(case, 24)
        for phase in ('baseline', 'fixed'):
            revision = helper._BASE_REVISION if phase == 'baseline' else helper._FIXED_REVISIONS[bug]
            record = provenance(revision)
            scope = helper.validated_quality_spill_scope([bug], phase, revision, record)
            bounded = helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                                                          scope=scope, runner=runner)
            check(bounded['spill_payload_file_policy']['production_revision'] == revision, 'revision')
            check(bounded['spill_payload_file_policy']['build_receipt_sha256'] == record['build_receipt_sha256'], 'receipt')
            check(bounded['table_service_overrides']['SpillingServiceConfig'] == helper.LOCAL_SERVICE, 'cap')
            rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata), 'unbound call')
            rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                      scope=scope, runner=temporary / 'wrong-runner'), 'wrong runner')
            rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                      scope=scope._replace(revision='f' * 40), runner=runner), 'wrong token revision')
            rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                      scope=scope._replace(bugs=('Q99',)), runner=runner), 'wrong finding token')
            rejected(lambda: helper.validated_quality_spill_scope([bug], phase, 'f' * 40, record), 'unknown revision')
            wrong = helper._FIXED_REVISIONS[bug] if phase == 'baseline' else helper._BASE_REVISION
            rejected(lambda: helper.validated_quality_spill_scope([bug], phase, wrong, record), 'phase inversion')
            bad = copy.deepcopy(record)
            bad['binary_provenance_verified'] = False
            rejected(lambda: helper.validated_quality_spill_scope([bug], phase, revision, bad), 'unverified runner')
            bad = copy.deepcopy(record)
            bad['build_receipt_sha256'] = 'b' * 64
            rejected(lambda: helper.validated_quality_spill_scope([bug], phase, revision, bad), 'receipt changed')
            bad = copy.deepcopy(record)
            bad['binary_provenance']['sha256'] = 'c' * 64
            rejected(lambda: helper.validated_quality_spill_scope([bug], phase, revision, bad), 'binary mismatch')
        rejected(lambda: helper.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                  scope={}, runner=runner), 'untyped token')

    check(helper.validated_quality_spill_scope(['Q02'], 'fixed', 'f' * 40, {}) is None, 'unaffected')
    check(helper.validated_quality_spill_scope(['Q99'], 'fixed', 'f' * 40, {}) is None, 'unknown grants no token')
    rejected(lambda: helper.bound_quality_spill_metadata(module, 'q99_spilling', 'new', 24,
              {'table_service_overrides': {'EnableQueryServiceSpilling': True}}), 'unknown spilling cell')
    revision = helper._BASE_REVISION
    record = provenance(revision)
    scope = helper.validated_quality_spill_scope(list(helper._FIXED_REVISIONS), 'baseline', revision, record)
    check(len(scope.bugs) == 14, 'batched baseline')
    rejected(lambda: helper.validated_quality_spill_scope(['Q03', 'Q04'], 'fixed',
              helper._FIXED_REVISIONS['Q03'], record), 'mixed fixed revisions')

    destination = temporary / 'rbo-fixes-20261023'
    destination.mkdir()
    (destination / 'native_verify.py').write_bytes((ROOT / 'native_verify.py').read_bytes())
    for patch in (original['patch'], bindings['patch']):
        result = subprocess.run(['git', 'apply', str(ROOT / patch)], cwd=temporary, capture_output=True)
        check(result.returncode == 0, 'temporary composition')
    for name, digest in bindings['after_images'].items():
        raw = (destination / name).read_bytes()
        ast.parse(raw)
        check(hashlib.sha256(raw).hexdigest() == digest, 'after image')
    tree = ast.parse((destination / 'native_verify.py').read_bytes())
    function = copy.deepcopy(next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'run_generated'))
    cutoff = next(i for i, n in enumerate(function.body) if isinstance(n, ast.Assign)
                  and any(isinstance(a, ast.Name) and a.id == 'query' for a in n.targets))
    function.body = function.body[:cutoff] + [ast.Return(value=ast.Name(id='meta', ctx=ast.Load()))]
    namespace = dict(live.__dict__)
    exec(compile(ast.fix_missing_locations(ast.Module(body=[function], type_ignores=[])),
                 'actual-native-preparation-prefix', 'exec'), namespace)
    module = live.import_generator_snapshot('quality_join_shuffle_checks')
    case = 'q14_reverse_chain_on'
    revision = helper._FIXED_REVISIONS['Q14']
    record = provenance(revision)
    scope = helper.validated_quality_spill_scope(['Q14'], 'fixed', revision, record)
    old_binary = live.g.BINARY
    live.g.BINARY = runner
    try:
        rejected(lambda: namespace['run_generated'](module, case, 'new', 24), 'actual unbound Q14 prefix')
        check(namespace['run_generated'](module, case, 'new', 24, spill_scope=scope)
              ['spill_payload_file_policy']['production_revision'] == revision, 'actual bound Q14 prefix')
    finally:
        live.g.BINARY = old_binary
    main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'main')
    calls = [(n.lineno, ast.unparse(n.func)) for n in ast.walk(main) if isinstance(n, ast.Call)]
    check(min(line for line, name in calls if name == 'verify_build_receipt')
          < min(line for line, name in calls if name == 'validated_quality_spill_scope')
          < min(line for line, name in calls if name == 'run_generated'), 'main ordering')
    calls = [n for n in ast.walk(main) if isinstance(n, ast.Call) and ast.unparse(n.func) == 'run_generated']
    check(len(calls) == 2 and any(any(k.arg == 'spill_scope' for k in n.keywords) for n in calls), 'target token forwarding')
    check(hashlib.sha256((ROOT / 'native_verify.py').read_bytes()).hexdigest() == original['native_before_sha256'], 'live unchanged')

report = dict(schema='quality_spill_revision_guard_metadata_contracts_v1', patch_sha256=bindings['patch_sha256'],
              checks_passed=checks, case_phase_pairs_checked=164, actual_native_preparation_prefix_checked=True,
              receipt_documents='Temporary synthetic metadata only; no runner executable or query invoked',
              native_build_graph_toolchain_runs=0, rendered_SQL_CPP_fixtures_or_plans_persisted=False,
              live_edits=False, script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
output = ROOT / 'verification/Q-SQL-spill-revision-guard-metadata-contracts-20261028.json'
output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(dict(report=str(output), sha256=hashlib.sha256(output.read_bytes()).hexdigest(), checks=checks)))
