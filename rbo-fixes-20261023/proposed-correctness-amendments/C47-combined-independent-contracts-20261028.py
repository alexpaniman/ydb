"""Independent source and metadata review; never executes a toolchain or generated program."""
import ast
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import ModuleType
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
DORMANT = ROOT / 'proposed-correctness-amendments'
CANDIDATES = DORMANT / 'C47-covering-import-driver'
sys.path.insert(0, str(ROOT))
from anonymous_cpp_metadata import MetadataError
CHECKS = 0
REVISION = '54db45bef39b3515bd662daefe9d10c8757e74db'


def check(value):
    global CHECKS
    if not value:
        raise AssertionError('Independent C47 source contract failed')
    CHECKS += 1


def rejects(function):
    try:
        function()
    except (ValueError, TypeError, KeyError, OSError, MetadataError):
        check(True)
    else:
        check(False)


def sha(value):
    return hashlib.sha256(value).hexdigest()


def load(path):
    module = ModuleType(path.stem)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def source(path):
    return subprocess.check_output(['git', 'show', REVISION + ':' + path], cwd=ROOT.parent)


direct_report_path = ROOT / 'verification/C47-covering-import-direct-source-preparation-20261028.json'
driver_report_path = ROOT / 'verification/C47-covering-import-direct-driver-source-preparation-20261028.json'
check(sha(direct_report_path.read_bytes()) == '133a2efafee9a06c4dd6e7b19860cf534d40c4cb3f274d6ef0689a96973feaf9')
check(sha(driver_report_path.read_bytes()) == 'e6404e22556ddaede6a98a00512eb9cd9f693ec85b1bbd5723de97b1fa5af0c9')
direct_report = json.loads(direct_report_path.read_bytes())
driver_report = json.loads(driver_report_path.read_bytes())
check(sha((ROOT / direct_report['generator']['patch']).read_bytes()) == direct_report['generator']['patch_sha256'])
check(sha((ROOT / direct_report['generator']['path']).read_bytes()) == direct_report['generator']['sha256'])
check(sha((ROOT / driver_report['patch']['path']).read_bytes()) == driver_report['patch']['sha256'])
for name, hashes in driver_report['files'].items():
    if hashes['before_sha256']:
        check(sha((ROOT / name).read_bytes()) == hashes['before_sha256'])
    check(sha((CANDIDATES / name).read_bytes()) == hashes['after_sha256'])
live_before = {name: (ROOT / name).read_bytes() for name in driver_report['files'] if (ROOT / name).exists()}
live_before['c47_lookup_copy_contract_generator.py'] = (ROOT / 'c47_lookup_copy_contract_generator.py').read_bytes()

for binding in direct_report['api_source_bindings']:
    raw = subprocess.check_output(['git', 'show', binding['revision'] + ':' + binding['path']], cwd=ROOT.parent)
    check(sha(raw) == binding['sha256'])
    # None of these API definitions changed in the final production amendment.
    check(source(binding['path']) == raw)

with tempfile.TemporaryDirectory(prefix='c47-independent-source-') as temporary:
    target = Path(temporary)
    for name, raw in live_before.items():
        (target / name).write_bytes(raw)
    for patch_name in ('C47-copy-generator-ordered-key-api-20261028.patch',
                       'C47-covering-import-direct-controls-20261028.patch',
                       'C47-covering-import-direct-driver-20261028.patch'):
        result = subprocess.run(['git', 'apply', '-p2', '--unsafe-paths', '--whitespace=error-all',
                                 str(DORMANT / patch_name)], cwd=target,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(result.returncode == 0)
    for name in (*driver_report['files'], 'c47_lookup_copy_contract_generator.py', 'c47_covering_import_contract_generator.py'):
        check((target / name).read_bytes() == (CANDIDATES / name).read_bytes())
    combined = load(target / 'c47_combined_cpp_generator.py')
    copy_module = load(target / 'c47_lookup_copy_contract_generator.py')
    covering = load(target / 'c47_covering_import_contract_generator.py')
    old_copy = ModuleType('original_copy')
    exec(compile(live_before['c47_lookup_copy_contract_generator.py'], '<copy-generator>', 'exec'), old_copy.__dict__)
    check(old_copy.cases() == copy_module.cases())
    check(old_copy.render_cpp_suite().replace('copied.LookupKeys.Size() == keys.Size()',
          'copied.LookupKeys.Items().size() == keys.Items().size()') == copy_module.render_cpp_suite())
    check(covering.cases() == direct_report['coverage']['positive'] + direct_report['coverage']['negative'])
    check(len(covering.cases()) == len(set(covering.cases())) == 32)
    check(len(direct_report['coverage']['positive']) == 6)
    check(len(direct_report['coverage']['negative']) == 26)
    check(covering.specs() == json.loads((ROOT / direct_report['execution_manifest']['path']).read_bytes())['exact_case_assertions'])
    specs = combined.specs()
    check(specs == driver_report['case_assertions'])
    check(combined.vector_manifest() == driver_report['vectors'])
    check(len(specs) == 51)
    check(sum(item['assertion_count'] for item in specs[:19]) == 249)
    check(sum(item['assertion_count'] for item in specs[19:]) == 547)
    check(sum(item['assertion_count'] for item in specs) == 796)
    rendered = combined.render_cpp_suite()
    check(sha(rendered.encode()) == driver_report['generated_source_sha256'])
    check(len(rendered.encode()) == driver_report['generated_source_bytes'])
    check(rendered == combined.render_cpp_suite())
    check(rendered.count('int main()') == 1)
    for label, module in [('copy', copy_module), ('covering', covering)]:
        standalone = module.render_cpp_suite()
        prefix = standalone.split('int main() { unsigned failures = 0;\n')[0]
        body = ''.join(line for line in prefix.splitlines(True) if not line.startswith('#include '))
        check(('namespace C47_' + label + ' {\n' + body + '}\n') in rendered)
        for index, name in enumerate(module.cases()):
            check(rendered.count('C47_' + label + '::Case' + str(index) + '();') == 1)
    for index, name in enumerate(copy_module.cases()):
        body = copy_module._body(name)
        loops = body.count('for (size_t index = 0;')
        check(loops in (0, 1))
        width = int(name.split('_')[2]) if name.startswith('plain_') and name.count('_') == 3 else 3
        expected = body.count('Check(') + (4 * (width - 1) if loops else 0)
        check(specs[index]['assertion_count'] == expected)
    for index, name in enumerate(covering.cases()):
        body = covering._body(name)
        check(specs[19 + index]['assertion_count'] == body.count('Check('))
        check(body.count('stage.RunStage(*root, f.RboCtx)') == 1)
        check(body.count('std::make_unique<TUseCoveringImportedIndexRule>()') == 1)
        if name in direct_report['coverage']['positive']:
            check('result.Get() != lookup.Get() && result.Get() != read.Get()' in body)
            check('typeRoot->ComputeTypes(f.RboCtx)' in body)
            check('replacement->GetColumns() == demanded' in body)
        else:
            check('Check(result.Get() == lookup.Get());' in body)
    check('NYql::EStorageType::RowStorage' in covering._prelude())
    check(', EStorageType::RowStorage,' not in covering._prelude())
    preserved = covering._body('preserved_read_state')
    check('TKqpReadTableSettings::Parse(TCoNameValueTupleList(replacement->ReadSettings)) == settings' not in preserved)
    for member in ('GetSorting()', 'ItemsLimit.Get()', 'ForcePrimary', 'SequentialInFlight'):
        check(member in preserved)
    check('pruning.RunStage(*root, f.RboCtx)' in covering._body('after_global_pruning'))
    check('map->GetMapElements().Keys().Size() == 4' in covering._body('after_global_pruning'))
    check('map->GetMapElements().Keys().Size() == 3' in covering._body('after_global_pruning'))
    check('lookup->Parents.size() == 2' in covering._body('shared_lookup_edges'))
    check('mergedPlan->GetInput(1).Get() == lookup.Get()' in covering._body('shared_lookup_edges'))
    check('read->Parents.size() == 2' in covering._body('shared_input_ownership'))
    check('mergedPlan->GetInput(1).Get() == read.Get()' in covering._body('shared_input_ownership'))
    check('lookup->Copy(f.Props, renames)' in covering._body('copied_definitions'))
    check('originalLookup->GetInput().Get() == originalRead.Get()' in covering._body('copied_definitions'))
    profiles = json.loads((target / 'correctness_cpp_profiles.json').read_bytes())
    check(profiles['profiles']['C47']['revision'] == REVISION)
    check(profiles['profiles']['C31'] == json.loads(live_before['correctness_cpp_profiles.json'])['profiles']['C31'])
    (target / 'cases').mkdir()
    (target / 'cases/C47.json').write_bytes((ROOT / 'cases/C47.json').read_bytes())
    check(json.loads((target / 'cases/C47.json').read_bytes())['commit'] == REVISION)
    contract = load(target / 'correctness_cpp_contract.py')
    check(contract.render_suite('C47', REVISION)[0] == rendered.encode())
    rows = [dict(case_index=i, checks_executed=item['assertion_count'], passed=True) for i, item in enumerate(specs)]
    encode = lambda values: b'\n'.join(json.dumps(value).encode() for value in values)
    check(contract.parse_verdicts(encode(list(reversed(rows))), 51, 0, bug='C47') == rows)
    for index, item in enumerate(rows):
        for field, value in [('checks_executed', item['checks_executed'] - 1),
                             ('checks_executed', item['checks_executed'] + 1),
                             ('checks_executed', True), ('checks_executed', 0),
                             ('case_index', True), ('case_index', -1), ('case_index', 51), ('passed', 1)]:
            modified = copy.deepcopy(rows)
            modified[index][field] = value
            rejects(lambda: contract.parse_verdicts(encode(modified), 51, 0, bug='C47'))
        modified = copy.deepcopy(rows)
        modified[index].update(passed=False, checks_executed=1)
        check(contract.parse_verdicts(encode(modified), 51, 1, bug='C47') == modified)
        rejects(lambda: contract.parse_verdicts(encode(modified), 51, 0, bug='C47'))
    for mutation in ('drop', 'duplicate', 'old_vector_only'):
        modified = rows[:-1] if mutation == 'drop' else rows + rows[:1] if mutation == 'duplicate' else rows[:19]
        rejects(lambda: contract.parse_verdicts(encode(modified), 51, 0, bug='C47'))
    rejects(lambda: contract.parse_verdicts(encode(rows), 51, False, bug='C47'))
    profile_path = target / 'correctness_cpp_profiles.json'
    for kind in ('case_name', 'case_order', 'assertions', 'component_hash', 'source_hash', 'revision'):
        modified = copy.deepcopy(profiles)
        value = modified['profiles']['C47']
        if kind == 'case_name': value['case_assertions'][19]['case'] += '_changed'
        elif kind == 'case_order': value['case_assertions'][19], value['case_assertions'][20] = value['case_assertions'][20], value['case_assertions'][19]
        elif kind == 'assertions': value['case_assertions'][19]['assertion_count'] += 1
        elif kind == 'component_hash': value['vectors'][1]['generator_sha256'] = 'a' * 64
        elif kind == 'source_hash': value['generated_source_sha256'] = 'a' * 64
        elif kind == 'revision': value['revision'] = 'a' * 40
        profile_path.write_text(json.dumps(modified))
        rejects(lambda: contract.render_suite('C47', REVISION))
    profile_path.write_text(json.dumps(profiles))

# Rerun the author's complete serial driver mocks, capturing the report in RAM.
author_metadata_path = ROOT / driver_report['metadata_validation']['path']
author_metadata_before = author_metadata_path.read_bytes()
check(sha(author_metadata_before) == driver_report['metadata_validation']['sha256'])
captured = []
original_write_text = Path.write_text


def capture_report(path, content, *args, **kwargs):
    if path == author_metadata_path:
        captured.append(content.encode())
        return len(content)
    return original_write_text(path, content, *args, **kwargs)


author = load(CANDIDATES / 'verify_metadata.py')
with patch.object(Path, 'write_text', capture_report), contextlib.redirect_stdout(io.StringIO()):
    author.main()
check(captured == [author_metadata_before])
check(author_metadata_path.read_bytes() == author_metadata_before)
for name, raw in live_before.items():
    check((ROOT / name).read_bytes() == raw)

report = {
    'status': 'source_and_metadata_approved_native_pending',
    'production_revision': REVISION,
    'independent_assertions': CHECKS, 'author_driver_checks_rerun': 274,
    'source_report_sha256': sha(direct_report_path.read_bytes()),
    'driver_report_sha256': sha(driver_report_path.read_bytes()),
    'patches': [
        {'path': direct_report['generator']['patch'], 'sha256': direct_report['generator']['patch_sha256']},
        driver_report['patch']],
    'case_count': 51, 'assertion_count': 796,
    'vectors': driver_report['vectors'], 'case_assertions': driver_report['case_assertions'],
    'generated_source_sha256': driver_report['generated_source_sha256'],
    'generated_source_bytes': driver_report['generated_source_bytes'],
    'resolved_generator_issues': [
        'Qualified NYql::EStorageType avoids ambiguity with the transitively included NKikimr::NKqp enum.',
        'Explicit read-setting field comparisons avoid deleted default equality on a base without equality.'
    ],
    'source_findings': [
        'All 19 existing copy contracts preserve their functions and meanings; only the reviewed ordered-key API spelling changed.',
        'All 32 covering controls call the real production rule through normal stage traversal; six replacements and 26 declines are asserted.',
        'Real loaded metadata supplies value types and optionality; positive outputs are typed with the actual Read typer.',
        'Shared source and duplicate consumer edges are checked through real parent-edge computation; replacements preserve distinct definition identities.',
        'Global pruning removes the unused mapped output before the covering stage; the normal local liveness pass supplies current demand.',
        'Full ordered primary keys, index identity/state/type, read settings/ranges/limit/sort, no-spill boundary and excluded lookup forms are represented.',
        'Combined component functions remain byte-identical in separate namespaces; one driver invokes each once.',
        'Exact source/component/vector/profile hashes and assertion counts reject omissions, short-circuited success and stale 19-case evidence.',
        'Production-version validation, closure receipt checks, serial compile/link/native sequence, existing scopes/timeouts/cleanup and anonymous transport remain unchanged.'
    ],
    'limits': [
        'Source and mocked transport checks do not establish C++ compilation or native correctness.',
        'Range controls check IR and expression identity, not runtime range evaluation.',
        'Excluded local IR markers are admission controls, not independent SQL-reachability claims.',
        '51 direct contracts remain separate from the existing 12 SQL semantic samples and unchanged covering physical assertion.',
        'The driver patch shares anonymous_cpp_verify.py with the pending C31 read-only executable transport repair; apply/rebase reviewed changes without dropping either.'
    ],
    'native_executed': False, 'build_or_toolchain_executed': False,
    'live_helpers_changed': False, 'generated_sources_persisted': False,
    'query_semantics_verified': False, 'quality_improvement_verified': False,
}
output = ROOT / 'verification/C47-combined-direct-independent-review-20261028.json'
output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'independent_assertions': CHECKS, 'author_driver_checks_rerun': 274,
                  'report_sha256': sha(output.read_bytes()), 'native_executed': False}))
