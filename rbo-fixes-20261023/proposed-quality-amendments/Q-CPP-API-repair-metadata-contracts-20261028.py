"""Source/metadata audit only. Generated C++ stays in RAM; never compile it."""
from pathlib import Path
from types import ModuleType
import copy
import hashlib
import json
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
CANDIDATES = Path(__file__).with_name('Q-CPP-API-repair-files-20261028')
CHECKS = 0


def check(value):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError('metadata contract failed')


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path):
    module = ModuleType(path.stem)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def rejects(fn):
    from anonymous_cpp_metadata import MetadataError
    try:
        fn()
    except (ValueError, MetadataError):
        check(True)
    else:
        check(False)


def main():
    sys.path.insert(0, str(ROOT))
    import anonymous_cpp_verify as direct
    from anonymous_cpp_metadata import MetadataError
    before_hashes = {name: digest((ROOT / name).read_bytes()) for name in (
        'quality_rule_cpp_generator.py', 'quality_statistics_cpp_generator.py',
        'quality_scheduler_cpp_checks.py', 'quality_statistics_cpp_profiles.json',
        'anonymous_cpp_verify.py', 'quality_statistics_cpp_contract.py')}
    expected_before = {
        'quality_rule_cpp_generator.py': '92a0d554eca03485322fac7b89e7b3f7f21962ec4bbc7aa7834e03b3de498674',
        'quality_statistics_cpp_generator.py': 'e26a52777a3b20789ecb1f892713d3f7883f111be9cc7334146d76c3378f9693',
        'quality_scheduler_cpp_checks.py': 'e32719455f2ea13b6cbd0b19772243c7d61f2497cfb9310c896515ecf642a00d',
        'quality_statistics_cpp_profiles.json': '9e2598a2879ac3626fb852cc6dc105c2614d95d195adee7838cb9c7890e6b273',
        'anonymous_cpp_verify.py': '4b4636dd8a448c279257b698d4c32e8b88cdd0f7956d6769a15f19e99b0f7873',
        'quality_statistics_cpp_contract.py': '7522d964569b2af5b9336a1003196d7efb633cdd705b5205ff059363300fe44d'}
    check(before_hashes == expected_before)
    metadata = {}
    repairs = {}
    untouched = {}
    for name, bugs in (
        ('quality_union_cpp_generator.py', ('Q11',)),
        ('quality_cpp_contract_generator.py', ('Q18', 'Q19')),
        ('quality_map_contract_generator.py', ('Q25',)),
        ('quality_ast_collection_generator.py', ('Q27',))):
        path = ROOT / name
        before = digest(path.read_bytes())
        module = load(path)
        for bug in bugs:
            if bug == 'Q27':
                names = ['q27_' + item['shape'] + '_' + str(item['depth']) for item in module.specs()]
                source_hashes = {revision: digest(module.render_cpp_suite(REPO, revision).encode())
                                 for revision in (module.BASE, module.FIXED)}
            else:
                names = module.cases() if bug == 'Q11' else [case for case in module.cases() if case.startswith(bug.lower() + '_')]
                # Q19 manglings are native-object inputs, discovered later.
                # Review its parameterized source generator, never substitute
                # invented native linkage evidence into the saved audit.
                source_hashes = None if bug == 'Q19' else {'current': digest((module.render_cpp_suite(bug)
                    if bug == 'Q18' else module.render_cpp_suite()).encode())}
            check(len(names) == len(set(names)))
            untouched[bug] = {'case_names': names, 'case_count': len(names),
                'generator_sha256': before, 'generated_source_sha256': source_hashes,
                'native_wrapper_symbols_pending': bug == 'Q19', 'source_or_profile_changes': False}
        check(digest(path.read_bytes()) == before)
    for name, bugs in (
        ('quality_rule_cpp_generator.py', ('Q14', 'Q16')),
        ('quality_statistics_cpp_generator.py', ('Q09', 'Q20')),
        ('quality_scheduler_cpp_checks.py', ('Q28',))):
        original = load(ROOT / name)
        candidate = load(CANDIDATES / name)
        check(original.cases() == candidate.cases())
        repairs[name] = {'before_sha256': before_hashes[name],
                         'after_sha256': digest((CANDIDATES / name).read_bytes())}
        for bug in bugs:
            names = [case for case in original.cases() if case.startswith(bug.lower() + '_')]
            old = original.render_cpp_suite() if bug == 'Q28' else original.render_cpp_suite(bug)
            new = candidate.render_cpp_suite() if bug == 'Q28' else candidate.render_cpp_suite(bug)
            expected = old
            if bug in ('Q14', 'Q20'):
                expected = expected.replace('EJoinAlgoType::', 'NKikimr::NKqp::EJoinAlgoType::')
            if bug == 'Q14':
                expected = expected.replace('ShuffleBy->Size()', 'ShuffleBy->Items().size()')
            if bug == 'Q28':
                expected = expected.replace('EStorageType::RowStorage', 'NYql::EStorageType::RowStorage')
            check(new == expected)
            check(new.count('Check(') == old.count('Check('))
            check(new.count('int main()') == old.count('int main()') == 1)
            check('TVector<TJoinColumn>' in new if bug == 'Q14' else True)
            counts = None
            if bug in ('Q09', 'Q20'):
                counts = candidate.expected_checks(bug)
                check(counts == original.expected_checks(bug))
                check(candidate.specs(bug) == original.specs(bug))
            if bug in ('Q14', 'Q16'):
                fn = candidate._q14 if bug == 'Q14' else candidate._q16
                old_fn = original._q14 if bug == 'Q14' else original._q16
                counts = [fn(case).count('Check(') for case in names]
                check(counts == [old_fn(case).count('Check(') for case in names])
                for case in names:
                    check(candidate.render_cpp(case).count('Check(') == original.render_cpp(case).count('Check('))
            if bug == 'Q28':
                check(candidate.specs() == original.specs())
                check(candidate.SCENARIOS == original.SCENARIOS)
            metadata[bug] = {'case_names': names, 'case_count': len(names),
                'expected_checks': counts, 'checks_total': sum(counts) if counts else None,
                'static_check_sites': new.count('Check(') - 1,
                'before_generated_source_sha256': digest(old.encode()),
                'after_generated_source_sha256': digest(new.encode()),
                'before_generated_source_bytes': len(old.encode()),
                'after_generated_source_bytes': len(new.encode()),
                'all_assertion_sites_and_case_vectors_preserved': True,
                'generated_source_persisted': False}
    old_profile = json.loads((ROOT / 'quality_statistics_cpp_profiles.json').read_bytes())
    new_profile = json.loads((CANDIDATES / 'quality_statistics_cpp_profiles.json').read_bytes())
    check(old_profile.keys() == new_profile.keys())
    for bug in ('Q09', 'Q20'):
        allowed = {'generator_sha256', 'generated_source_sha256', 'generated_source_bytes'}
        check({k: v for k, v in old_profile[bug].items() if k not in allowed}
              == {k: v for k, v in new_profile[bug].items() if k not in allowed})
        check(new_profile[bug]['generator_sha256'] == repairs['quality_statistics_cpp_generator.py']['after_sha256'])
        check(new_profile[bug]['generated_source_sha256'] == metadata[bug]['after_generated_source_sha256'])
    check(old_profile['Q09']['generated_source_sha256'] == new_profile['Q09']['generated_source_sha256'])
    with tempfile.TemporaryDirectory(prefix='rbo-api-source-review-') as directory:
        temp = Path(directory)
        install = temp / 'install'
        (install / 'rbo-fixes-20261023').mkdir(parents=True)
        for path in CANDIDATES.iterdir():
            (install / 'rbo-fixes-20261023' / path.name).write_bytes((ROOT / path.name).read_bytes())
        for bug in ('Q14', 'Q20', 'Q28'):
            patch = ROOT / 'proposed-quality-amendments' / (bug + '-direct-generator-API-repair-20261028.patch')
            result = subprocess.run(['git', 'apply', '--check', str(patch)], cwd=install,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            check(result.returncode == 0)
            result = subprocess.run(['git', 'apply', str(patch)], cwd=install,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            check(result.returncode == 0)
        for path in CANDIDATES.iterdir():
            check((install / 'rbo-fixes-20261023' / path.name).read_bytes() == path.read_bytes())
        for path in CANDIDATES.glob('*.py'):
            (temp / path.name).write_bytes(path.read_bytes())
        profile_path = temp / 'quality_statistics_cpp_profiles.json'
        profile_path.write_bytes((CANDIDATES / profile_path.name).read_bytes())
        (temp / 'cases').mkdir()
        for bug in ('Q09', 'Q20'):
            (temp / 'cases' / (bug + '.json')).write_bytes((ROOT / 'cases' / (bug + '.json')).read_bytes())
        contract = load(ROOT / 'quality_statistics_cpp_contract.py')
        contract.ROOT = temp
        for bug in ('Q09', 'Q20'):
            module, generator_hash, vector = contract.current_contract(bug)
            check(generator_hash == repairs['quality_statistics_cpp_generator.py']['after_sha256'])
            check(vector['case_names'] == metadata[bug]['case_names'])
            records = [dict(case_index=i, checks_executed=n, passed=True)
                       for i, n in enumerate(module.expected_checks(bug))]
            contract.check_records(records, bug)
            check(True)
            for index in range(len(records)):
                bad = copy.deepcopy(records)
                bad[index]['checks_executed'] -= 1
                rejects(lambda: contract.check_records(bad, bug))
            for field in ('generator_sha256', 'generated_source_sha256', 'generated_source_bytes',
                          'case_names', 'expected_checks', 'specifications_sha256'):
                bad = copy.deepcopy(new_profile)
                value = bad[bug][field]
                bad[bug][field] = [] if isinstance(value, list) else -1 if isinstance(value, int) else '0' * 64
                profile_path.write_text(json.dumps(bad))
                rejects(lambda: contract.current_contract(bug))
                profile_path.write_text(json.dumps(new_profile))
        saved_here = direct.HERE
        try:
            direct.HERE = temp
            for bug in ('Q14', 'Q16', 'Q28'):
                module, generator_hash, vector = direct._current_pair_contract(bug)
                check(vector['case_names'] == metadata[bug]['case_names'])
                check(generator_hash == repairs[direct.GENERATORS[bug] + '.py']['after_sha256'])
                stale = {'generator_sha256': before_hashes[direct.GENERATORS[bug] + '.py'],
                         'case_count': len(vector['case_names']),
                         'direct_case_vector_sha256': direct._json_digest(vector)}
                try:
                    direct._validate_saved_pair_contract(stale, bug, 'unused', module, generator_hash, vector)
                except MetadataError as error:
                    check(str(error) == 'saved_direct_pair_adapter_or_coverage_changed')
                else:
                    check(False)
        finally:
            direct.HERE = saved_here
    source_files = (
        'ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h',
        'ydb/core/kqp/opt/rbo/kqp_operator.h',
        'ydb/core/kqp/opt/rbo/kqp_info_unit.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_rules.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_cbo.h',
        'ydb/core/kqp/opt/rbo/kqp_rbo_utils.h',
        'ydb/core/kqp/opt/cbo/cbo_optimizer_new.h',
        'ydb/core/kqp/opt/cbo/kqp_statistics.h',
        'ydb/core/kqp/opt/kqp_column_statistics_utils.h',
        'yql/essentials/core/yql_cost_function.h',
        'yql/essentials/core/yql_statistics.h',
        'yql/essentials/core/histogram/eq_width_histogram.h',
        'yql/essentials/core/histogram/eq_height_histogram_reader.h',
        'yql/essentials/core/minsketch/count_min_sketch.h')
    revisions = {}
    source_bindings = {}
    for bug in ('Q09', 'Q11', 'Q14', 'Q16', 'Q18', 'Q19', 'Q20', 'Q25', 'Q27', 'Q28'):
        case = json.loads((ROOT / 'cases' / (bug + '.json')).read_bytes())
        revisions[bug] = {key: case[key] for key in ('base', 'commit')}
        for revision in (case['base'], case['commit']):
            if revision in source_bindings:
                continue
            files = {}
            sources = {}
            for path in source_files:
                raw = subprocess.check_output(['git', 'show', revision + ':' + path], cwd=REPO)
                files[path] = digest(raw)
                sources[path] = raw.decode()
            check('enum class EJoinAlgoType' in sources[source_files[7]])
            check('enum class EJoinAlgoType' in sources[source_files[10]])
            check('using namespace NYql;' in sources[source_files[1]])
            check('const NYql::EStorageType storageType' in sources[source_files[1]])
            check('enum EStorageType' in sources[source_files[8]])
            check('enum EStorageType' in sources[source_files[11]])
            ordered = sources[source_files[2]].split('class TOrderedIUs', 1)[1].split('class TPairedIUCollection', 1)[0]
            check('Items() const&' in ordered)
            check(not re.search(r'\bSize\s*\(', ordered))
            check('std::optional<TOrderedIUs<>> LeftShuffleBy' in sources[source_files[1]])
            check('std::optional<TOrderedIUs<>> RightShuffleBy' in sources[source_files[1]])
            source_bindings[revision] = files
    for name, before in before_hashes.items():
        check(digest((ROOT / name).read_bytes()) == before)
    result = {'status': 'source_metadata_only_passed', 'checks': CHECKS,
        'repairs': repairs, 'coverage': metadata, 'unchanged_audit': untouched, 'revisions': revisions,
        'source_api_sha256_by_revision': source_bindings,
        'profile_before_sha256': before_hashes['quality_statistics_cpp_profiles.json'],
        'profile_after_sha256': digest((CANDIDATES / 'quality_statistics_cpp_profiles.json').read_bytes()),
        'admission_helper_sha256': {name: before_hashes[name] for name in ('anonymous_cpp_verify.py', 'quality_statistics_cpp_contract.py')},
        'native_runs': 0, 'compiler_runs': 0, 'build_runs': 0,
        'production_or_live_helper_changes': False, 'generated_source_persisted': False,
        'local_contracts_passed': False, 'query_semantics_verified': False,
        'quality_improvement_verified': False, 'quality_finding_fully_verified': False}
    report = ROOT / 'verification' / 'Q-CPP-API-repair-metadata-contracts-20261028.json'
    report.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'checks': CHECKS, 'report_sha256': digest(report.read_bytes()), 'native_runs': 0}))


if __name__ == '__main__':
    main()
