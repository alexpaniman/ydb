"""Independent source and metadata checks; no runner/build/Git mutation."""
from pathlib import Path
import ast
import contextlib
import copy
import hashlib
import io
import json
import subprocess
from types import ModuleType
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT.parent
DIRECTORY = ROOT / 'proposed-correctness-amendments/C06-C50-bounded-integration-20261028'
GENERATOR = DIRECTORY / 'c06_c50_bounded_integration_checks.py'
REPORT = ROOT / 'verification/C06-C50-bounded-integration-source-preparation-20261028.json'
checks = 0


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def check(value, label):
    global checks
    checks += 1
    if not value:
        raise AssertionError(label)


def git(*args):
    return subprocess.check_output(['git', *args], cwd=PROJECT)


source = GENERATOR.read_bytes()
tree = ast.parse(source)
assignments = {}
for node in tree.body:
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
        try:
            assignments[node.targets[0].id] = ast.literal_eval(node.value)
        except (TypeError, ValueError):
            pass
report = json.loads(REPORT.read_bytes())
check(sha(REPORT.read_bytes()) == 'dbc0cafd5431d11991d8f8fa29b2d12320d42a7e11f9561bc606a55acbdc6ff4', 'frozen report')
check(sha(source) == report['new_module_sha256'], 'frozen generator')
check(sha((ROOT / report['patch']).read_bytes()) == report['patch_sha256'], 'frozen patch')
comparison = json.loads((ROOT / report['source_comparison']['path']).read_bytes())
check(sha((ROOT / report['source_comparison']['path']).read_bytes()) == report['source_comparison']['sha256'], 'comparison source identity')
base = comparison['base']
changed = comparison['changed_files']
revisions = sorted({value['revision'] for value in changed.values()})
actual_changed = {}
for revision in revisions:
    check(git('rev-parse', revision + '^').decode().strip() == base, 'independent main parent')
    paths = git('diff', '--name-only', base, revision).decode().splitlines()
    for name in paths:
        check(name not in actual_changed, 'disjoint integration diff')
        actual_changed[name] = revision
check(actual_changed == {name: value['revision'] for name, value in changed.items()}, 'exact integration changed paths')
for name, digest in assignments['PRODUCTION_BINDINGS'].items():
    revision = actual_changed.get(name, base)
    check(sha(git('show', revision + ':' + name)) == digest, 'production source digest')
for name, digest in assignments['SOURCE_BINDINGS'].items():
    check(sha((PROJECT / name).read_bytes()) == digest, 'artifact source digest')

# Recompute only the changed ancestor tree objects in memory. No Git object is
# created and no index/worktree is touched.
directories = {''}
for name in changed:
    parent = Path(name).parent
    while str(parent) != '.':
        directories.add(str(parent))
        parent = parent.parent
entries = {}
for directory in directories:
    object_name = base + (':' + directory if directory else '^{tree}')
    raw = git('ls-tree', '-z', object_name)
    values = []
    for item in raw.split(b'\0'):
        if not item:
            continue
        descriptor, name = item.split(b'\t', 1)
        mode, kind, oid = descriptor.split(b' ', 2)
        values.append((mode, kind, name, oid.decode()))
    entries[directory] = values


def compute(replace):
    hashes = {}
    for directory in sorted(directories, key=lambda p: len(p.split('/')) if p else 0, reverse=True):
        data = b''
        for mode, kind, name, oid in entries[directory]:
            path = (directory + '/' if directory else '') + name.decode()
            if replace and path in changed:
                oid = git('rev-parse', actual_changed[path] + ':' + path).decode().strip()
            elif kind == b'tree' and path in hashes:
                oid = hashes[path]
            data += (mode.lstrip(b'0') + b' ' + name + b'\0' + bytes.fromhex(oid))
        hashes[directory] = hashlib.sha1(b'tree ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    return hashes['']


check(compute(False) == git('rev-parse', base + '^{tree}').decode().strip(), 'tree hash base self-check')
check(compute(True) == assignments['EXPECTED_TREE'] == comparison['exact_expected_integration_tree'], 'independent exact combined tree')

original = json.loads((ROOT / report['original_source_preparation']['path']).read_bytes())
records = assignments['RECORDS']
check(len(records) == 16 and len(assignments['CASES']) == 8, 'exact counts')
check([(x['case'], x['repetition']) for x in records] == [(case, i) for case in assignments['CASES'] for i in (0, 1)], 'exact ordered repeats')
for expected, old in zip(records, original['expected_records']):
    for key in ('query_sha256', 'schema_sha256', 'fixture_sha256'):
        check(expected[key] == old[key], 'retained workload fingerprint')
    check(expected['settings_sha256'] != old['settings_sha256'], 'intentional setting change')
    for key, value in (('rows', 24), ('node_count', 4), ('mode', 'new')):
        check(expected[key] == value, 'exact execution dimensions')

# Re-run the author's frozen metadata tests, capturing only its sanitized report
# in RAM instead of overwriting the already reviewed report file.
tests = ModuleType('c06_c50_author_metadata_source_only')
tests.__file__ = str(DIRECTORY / 'verify_metadata.py')
exec(compile(Path(tests.__file__).read_bytes(), tests.__file__, 'exec'), tests.__dict__)
original_write = Path.write_text
captured = {}
target = ROOT / 'verification/C06-C50-bounded-integration-metadata-contracts-20261028.json'


def write(self, contents, *args, **kwargs):
    if self == target:
        captured['report'] = json.loads(contents)
        return len(contents)
    return original_write(self, contents, *args, **kwargs)


with patch.object(Path, 'write_text', write), contextlib.redirect_stdout(io.StringIO()):
    tests.main()
check(captured['report']['checks_passed'] == 644, 'author metadata regression assertions')
check(sha(target.read_bytes()) == report['validation']['sha256'], 'frozen author report unchanged')
result = dict(schema_version='c06_c50_independent_source_metadata_review_v1',
    status='generator_source_checks_passed_dedicated_runner_and_final_binding_refresh_pending',
    source_assertions=checks, author_regression_assertions=644,
    reviewed_generator_sha256=sha(source), generator_patch_sha256=report['patch_sha256'],
    exact_combined_tree=assignments['EXPECTED_TREE'], independently_recomputed_tree=True,
    production_sources_checked=len(assignments['PRODUCTION_BINDINGS']), artifact_sources_checked=len(assignments['SOURCE_BINDINGS']),
    ordered_records=16, retained_workload_slots=48, intentionally_changed_settings_slots=16,
    no_native_build_or_graph_runs=True, no_Git_mutations=True, no_live_helper_edits=True,
    rendered_workloads_or_results_persisted=False, native_admission=False,
    remaining=['Review dedicated runner failure retention and fresh exact guard binding.',
               'Review narrow refresh to native_verify final idle53 after-image before application.',
               'Actual integration receipt, guarded execution and exact16 admission remain pending.'])
out = ROOT / 'verification/C06-C50-bounded-generator-independent-source-checks-20261028.json'
out.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
print(json.dumps(dict(report=str(out.relative_to(ROOT)), sha256=sha(out.read_bytes()), source_assertions=checks,
                      author_regression_assertions=644)))
