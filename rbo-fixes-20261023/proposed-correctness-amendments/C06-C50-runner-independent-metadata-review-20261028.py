"""Independent frozen runner review, using source/metadata only."""
from pathlib import Path
import ast
import contextlib
import copy
import hashlib
import io
import json
import subprocess
import sys
import tempfile
from types import ModuleType
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT.parent
CANDIDATE = ROOT / 'proposed-correctness-amendments/C06-C50-integration-runner-20261028'
REPORT_PATH = ROOT / 'verification/C06-C50-bounded-integration-runner-source-preparation-20261028.json'
REPORT_HASH = 'a5faa69fae691c783368e8eaa8f3214c90fdb1cdfd50ed9210bd87aaa1d66492'
CHECKS = 0


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def check(ok, label):
    global CHECKS
    CHECKS += 1
    if not ok:
        raise AssertionError(label)


def rejected(fn, label):
    try:
        fn()
    except (ValueError, TypeError, KeyError, OSError):
        check(True, label)
        return
    check(False, label)


check(sha(REPORT_PATH.read_bytes()) == REPORT_HASH, 'frozen report')
report = json.loads(REPORT_PATH.read_bytes())
composition = json.loads((ROOT / 'verification/quality-idle53-source-composition-20261028.json').read_bytes())
old_raw = (ROOT / 'proposed-correctness-amendments/C06-C50-bounded-integration-20261028/c06_c50_bounded_integration_checks.py').read_bytes()
new_raw = (CANDIDATE / 'c06_c50_bounded_integration_checks.py').read_bytes()
runner_raw = (CANDIDATE / 'c06_c50_integration_runner.py').read_bytes()
check(sha(runner_raw) == report['runner_patch']['after_sha256'], 'frozen runner source')
check(sha(new_raw) == report['binding_refresh_patch']['after_sha256'], 'refreshed generator')
old_ast, new_ast = ast.parse(old_raw), ast.parse(new_raw)


def take_binding(tree):
    item = next(node for node in tree.body if isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == 'SOURCE_BINDINGS' for target in node.targets))
    tree.body.remove(item)
    return ast.literal_eval(item.value)


old_bindings, new_bindings = take_binding(old_ast), take_binding(new_ast)
check(ast.dump(old_ast) == ast.dump(new_ast), 'only source binding assignment changed')
check({key for key in old_bindings.keys() | new_bindings.keys() if old_bindings.get(key) != new_bindings.get(key)}
      == {'rbo-fixes-20261023/native_verify.py', 'rbo-fixes-20261023/quality_spill_bounds.py'}, 'exact dependency delta')
for name in ('anonymous_cpp_verify.py', 'native_verify.py', 'quality_spill_bounds.py'):
    key = ROOT.name + '/' + name
    check(report['source_bindings'][key] == composition['after_images'][key] == sha((CANDIDATE / name).read_bytes()),
          'approved idle53 exact after-image')
check(set(new_bindings) <= set(report['source_bindings']), 'runner binds every generator dependency')
for key, value in new_bindings.items():
    check(report['source_bindings'][key] == value, 'same generator dependency digest')

with tempfile.TemporaryDirectory(prefix='c06-c50-independent-runner-source-') as td:
    project = Path(td).resolve()
    root = project / ROOT.name
    root.mkdir()
    for name, expected in report['source_bindings'].items():
        if name == ROOT.name + '/c06_c50_bounded_integration_checks.py':
            continue
        source = CANDIDATE / Path(name).name
        if not source.exists():
            source = PROJECT / name
        raw = source.read_bytes()
        check(sha(raw) == expected, 'all21 source identities')
        dest = project / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(raw)
    patches = [
        ('rbo-fixes-20261023/proposed-correctness-amendments/C06-C50-bounded-integration-generator-20261028.patch',
         '75ea46631d821354dde8ca87aee6d9845a612c09626fb6fefb52e62b24ae15a6'),
        (report['binding_refresh_patch']['path'], report['binding_refresh_patch']['sha256']),
        (report['runner_patch']['path'], report['runner_patch']['sha256'])]
    for name, expected in patches:
        source = PROJECT / name
        check(sha(source.read_bytes()) == expected, 'frozen composition patch')
        result = subprocess.run(['git', 'apply', str(source)], cwd=project, capture_output=True)
        check(result.returncode == 0, 'exact disposable ordered application')
    check((root / 'c06_c50_bounded_integration_checks.py').read_bytes() == new_raw, 'composed generator bytes')
    runner_path = root / 'c06_c50_integration_runner.py'
    check(runner_path.read_bytes() == runner_raw, 'composed runner bytes')
    runner = ModuleType('independent_c06_c50_runner')
    runner.__file__ = str(runner_path)
    exec(compile(runner_raw, str(runner_path), 'exec'), runner.__dict__)
    images = runner._sources()
    check(len(images) == 21, 'complete source set')
    for name in runner.SOURCE_BINDINGS:
        path = project / name
        raw = path.read_bytes()
        path.write_bytes(raw + b'\n')
        try:
            rejected(runner._sources, 'any21 source drift rejected')
        finally:
            path.write_bytes(raw)
    driver = runner._driver(images, project / 'synthetic-worktree')
    loaded_names = ('generate_and_verify', 'correlation_oracle_generator', 'partial_lookup_key_generator',
                    'optimizer_mode_contract', 'verification_contract', 'binary_provenance', 'quality_spill_bounds', 'native_verify')
    for name in loaded_names:
        module = sys.modules[name]
        key = str(Path(module.__file__).relative_to(project))
        check(module._verification_source_sha256 == runner.SOURCE_BINDINGS[key], 'actual loaded dependency source')
    adapter = sys.modules['quality_spill_bounds']
    generator = runner._generator(images)
    for cell in generator.CASES:
        metadata = {'table_service_config': {'EnableQueryServiceSpilling': True, 'EnableSpillingNodes': 'None'}}
        check(adapter.bound_quality_spill_metadata(None, cell, 'new', 24, metadata) is metadata,
              'final quality adapter preserves integration path')
    check(sha(runner.canonical(list(generator.RECORDS))) == runner.VECTOR_SHA256, 'actual exact16 vector binding')

    spec = {'run_id': 'a' * 32, 'nonce': 'b' * 64}
    identity = sha(runner.canonical(spec))
    common = dict(run_id=spec['run_id'], nonce=spec['nonce'], identity_sha256=identity)
    envelopes = [dict(kind='start', **common)]
    for index, cell in enumerate(generator.RECORDS):
        record = dict(cell, integration_run_id=spec['run_id'], integration_nonce=spec['nonce'], integration_record_index=index)
        envelopes.append(dict(kind='record', index=index, record=record, **common))
    envelopes.append(dict(kind='complete', completed_records=16, **common))

    def parse(items):
        return runner._parse_slice(b'\n'.join(runner.canonical(item) for item in items) + b'\n', spec)

    records, start, complete, error = parse(envelopes)
    check(len(records) == 16 and start and complete and error is None, 'exact metadata stream parsed')
    for index in range(16):
        for field in ('nonce', 'run_id', 'identity_sha256'):
            mutated = copy.deepcopy(envelopes)
            mutated[index + 1][field] = '0' * len(mutated[index + 1][field])
            records, _, complete, error = parse(mutated)
            check(len(records) == index and not complete and error is not None, 'invalid envelope retains only verified prefix')
        mutated = copy.deepcopy(envelopes)
        mutated[index + 1]['record']['integration_record_index'] = 99
        records, _, complete, error = parse(mutated)
        check(len(records) == index and not complete and error is not None, 'wrong record index rejected')
    for end in range(18):
        records, _, complete, error = parse(envelopes[:end])
        check(not complete and len(records) == max(0, end - 1), 'truncated valid prefix cannot complete')

    # Top-level local imports for every executed dependency are checked. Lazy
    # direct-suite/Q01 measurement dispatches are not invoked by this runner.
    top_imports = {}
    for name in loaded_names + ('anonymous_cpp_guard', 'anonymous_cpp_metadata', 'anonymous_cpp_verify', 'memory_safe_build'):
        directory = 'rbo-review-20261021' if name in loaded_names[:3] else ROOT.name
        file = project / directory / (name + '.py')
        imports = []
        for node in ast.parse(file.read_bytes()).body:
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else [node.module] if isinstance(node, ast.ImportFrom) else []
            for imported in names:
                for folder in (project / ROOT.name, project / 'rbo-review-20261021'):
                    dependency = folder / ((imported or '').split('.')[0] + '.py')
                    if dependency.is_file():
                        relative = str(dependency.relative_to(project))
                        check(relative in runner.SOURCE_BINDINGS, 'executed local import bound')
                        imports.append(relative)
        top_imports[name] = sorted(set(imports))

# Run frozen author mocks without rewriting their frozen evidence report.
test_path = PROJECT / report['metadata_validation']['script']['path']
check(sha(test_path.read_bytes()) == report['metadata_validation']['script']['sha256'], 'frozen regression script')
tests = ModuleType('independent_author_runner_regression')
tests.__file__ = str(test_path)
exec(compile(test_path.read_bytes(), str(test_path), 'exec'), tests.__dict__)
target = PROJECT / report['metadata_validation']['path']
captured = {}
original_write = Path.write_text


def write(self, value, *args, **kwargs):
    if self == target:
        captured['report'] = json.loads(value)
        return len(value)
    return original_write(self, value, *args, **kwargs)


with patch.object(Path, 'write_text', write), contextlib.redirect_stdout(io.StringIO()):
    tests.main()
check(captured['report']['checks_passed'] == 795 and captured['report']['scenario_count'] == 40, 'all frozen regressions pass')
check(sha(target.read_bytes()) == report['metadata_validation']['sha256'], 'frozen metadata evidence unchanged')
out = ROOT / 'verification/C06-C50-bounded-integration-independent-runner-review-20261028.json'
result = dict(schema_version='c06_c50_independent_runner_review_v1', status='approved_source_and_mock_scope_only',
    reviewed_report_sha256=REPORT_HASH, runner_patch_sha256=report['runner_patch']['sha256'],
    binding_refresh_patch_sha256=report['binding_refresh_patch']['sha256'],
    original_generator_patch_sha256=patches[0][1], runner_after_sha256=sha(runner_raw),
    generator_after_sha256=sha(new_raw), independent_checks=CHECKS, author_mock_checks=795, author_mock_scenarios=40,
    executed_top_level_local_imports=top_imports, source_binding_count=21,
    prior_generator_review=dict(path='verification/C06-C50-bounded-generator-independent-source-checks-20261028.json',
        sha256=sha((ROOT / 'verification/C06-C50-bounded-generator-independent-source-checks-20261028.json').read_bytes())),
    reviewed_correction='Completed native records are now sanitized and emitted before propagating a validation interruption.',
    transport=['Fresh readonly sealed manifest binds nonce, exact source set, tree, revision, receipt and ordered vector.',
        'The existing serial lease covers the guarded worker and finalization; only the worker invokes the database serially.',
        'Current guard return and stdout hash/size bind fresh memory evidence; no caller-selected old guard document is accepted.',
        'Valid completed-record prefixes persist before final executable/source validation; malformed or incomplete output cannot pass.',
        'All records use exact8-case two-repeat vector, rows24, four nodes; original48 workload fingerprints and16 changed settings remain exact.'],
    limits=['Source/mock approval is not native integration evidence.',
        'Root must install exact idle53 plus original generator, binding refresh and revised runner, then supply the fresh exact integration build receipt.',
        'Abrupt process death preserves only already emitted records.',
        'Auxiliary UDF identity is checked, but a separate successful UDF build receipt is not claimed.',
        'No final distribution proof, standalone finding admission, original-failure reclassification or unchanged-configuration claim.'],
    native_build_graph_runs=0, Git_mutations=0, live_source_edits=0, rendered_workloads_or_results_persisted=False,
    native_admission=False)
out.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
print(json.dumps(dict(path=str(out.relative_to(ROOT)), sha256=sha(out.read_bytes()), checks=CHECKS,
                      author_mock_checks=795)))
