"""Exercise composed generator/configuration contracts in RAM only."""
import ast
import copy
import hashlib
import importlib
import importlib.abc
import importlib.util
import json
from pathlib import Path
import sys
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROJECT = ROOT.parent
REPORT_PATH = ROOT / 'verification/quality-idle60-ordered-source-composition-20261028.json'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class Sources(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    def __init__(self, virtual):
        self.virtual, self.paths = virtual, {}

    def find_spec(self, fullname, path=None, target=None):
        if '.' in fullname:
            return None
        for folder in (ROOT, PROJECT / 'rbo-review-20261021'):
            candidate = (folder / (fullname + '.py')).resolve()
            if candidate in self.virtual or candidate.is_file():
                self.paths[fullname] = candidate
                return importlib.util.spec_from_loader(fullname, self, origin=str(candidate))
        return None

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        path = self.paths[module.__name__]
        raw = path.read_bytes()
        module.__file__ = str(path)
        module._verification_source_sha256 = sha(raw)
        exec(compile(raw, str(path), 'exec'), module.__dict__)


def main():
    source_report = json.loads(REPORT_PATH.read_bytes())
    virtual = {(PROJECT / name).resolve(): (HERE / 'after-images' / Path(name).name).read_bytes()
               for name in source_report['after_images']}
    checks, renders, vectors = [], {}, {}
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    for name, expected in source_report['after_images'].items():
        check(sha(virtual[(PROJECT / name).resolve()]) == expected, 'exact_final_image:' + name)
    old_read_bytes, old_isfile = Path.read_bytes, Path.is_file
    def read(path):
        return virtual[path.resolve()] if path.resolve() in virtual else old_read_bytes(path)
    def isfile(path):
        return path.resolve() in virtual or old_isfile(path)
    finder = Sources(virtual)
    sys.meta_path.insert(0, finder)
    with patch.object(Path, 'read_bytes', read), patch.object(Path, 'is_file', isfile):
        pairing = importlib.import_module('quality_pairing')
        for label, spec in source_report['profiles'].items():
            check(pairing.profile_fingerprint(spec['modules']) == spec['after_sha256'], 'actual_recursive_profile:' + label)

        runtime = importlib.import_module('quality_range_runtime_cpp_generator')
        runtime_source = runtime.render_cpp_suite().encode()
        check(len(runtime.specs()) == 42, 'Q01_runtime42')
        check(sha(runtime_source) == '43097b15ca565006cb3def2756b238c877fa224b58bb138a8cb587f53adaec49',
              'Q01_reviewed_allocator_instrumented_render')
        renders['Q01_runtime42'] = {'sha256': sha(runtime_source), 'bytes': len(runtime_source)}
        vectors['Q01_runtime42'] = [dict(spec) for spec in runtime.specs()]
        del runtime_source
        canonical = pairing.selected_cases('Q01', pairing.generator_modules('Q01'), 24)
        check(len(canonical) == 652, 'Q01_exact_canonical652')
        vectors['Q01_canonical652'] = list(canonical)
        stage = importlib.import_module('quality_q01_concrete_contract')
        frozen = stage.manifest(2048, 'a' * 32)
        check(len(frozen['profiles']) == 15 and len(frozen['schedule']) == 180, 'Q01_closed15_resource180')
        check(frozen['direct_runtime_cases_required'] == 42 and frozen['canonical_SQL_cases_required'] == 652,
              'Q01_required42_and652_preserved')
        check(frozen['optimization_acceptance_enabled'] is False, 'Q01_stageA_cannot_promote_quality')
        verdict = stage.validate_stage_A([], frozen)
        check(verdict['semantic_path_pass'] is False, 'Q01_empty_path_vector_rejected')
        check(all(value is False for key, value in verdict.items() if 'optimization' in key and isinstance(value, bool)),
              'Q01_empty_path_never_quality_success')
        vectors['Q01_finite15'] = [item['case'] for item in frozen['profiles']]
        vectors['Q01_pilot30'] = [{'case': case, 'phase': phase} for case in vectors['Q01_finite15']
                                  for phase in ('baseline', 'fixed')]
        vectors['Q01_resource180'] = frozen['schedule']

        rules = importlib.import_module('quality_rule_cpp_generator')
        q14 = importlib.import_module('quality_join_shuffle_checks')
        vectors['Q14_direct12'] = [case for case in rules.cases() if case.startswith('q14_')]
        vectors['Q14_SQL6'] = list(q14.case_specs())
        check(len(vectors['Q14_direct12']) == 12 and len(vectors['Q14_SQL6']) == 6, 'Q14_exact_direct12_SQL6')
        source = rules.render_cpp_suite('Q14').encode()
        renders['Q14_direct12'] = {'sha256': sha(source), 'bytes': len(source)}
        del source
        for case in vectors['Q14_SQL6']:
            check(q14.metadata(case, 24)['node_count'] == 4, 'Q14_four_node_scope:' + case)
            check(q14.pair_observations({'case': case}, {'case': case}, source_report['new_Q28_revision']['parent'],
                                       '0600104a1fba93917e7a4e11a4f1b2ad0198cdbd',
                                       sha(Path(q14.__file__).read_bytes()),
                                       source_report['profiles']['Q14']['after_sha256']) is False,
                  'Q14_empty_or_unbound_evidence_rejected:' + case)

        original = importlib.import_module('quality_scheduler_cpp_checks')
        extra = importlib.import_module('quality_scheduler_depth_checks')
        driver = importlib.import_module('q28_depth_verify')
        sdk = importlib.import_module('q28_compilation_measurement')
        scheduler = importlib.import_module('quality_scheduler_semantic_checks')
        vectors['Q28_original26'] = original.specs()
        vectors['Q28_additional4'] = extra.specs()
        vectors['Q28_SQL8'] = list(scheduler.recipes(24))
        vectors['Q28_SDK7'] = list(sdk.EXECUTED_CASES)
        check(len(vectors['Q28_original26']) == 26 and len(vectors['Q28_additional4']) == 4, 'Q28_required26_separate4')
        for label, module, expected in (
                ('Q28_original26', original, 'cf9105de708101ba9972f78b3bc9ba22d06eca7e581e28bf2470a7e1eca31bf7'),
                ('Q28_additional4', extra, 'a33b908be2ad18993b390c8cc970040f29709c4dc62d6f2c360417b48953788a')):
            source = module.render_cpp_suite().encode()
            check(sha(source) == expected, 'unchanged_Q28_render:' + label)
            renders[label] = {'sha256': sha(source), 'bytes': len(source)}
            del source
        check(len(vectors['Q28_SQL8']) == 8 and len(vectors['Q28_SDK7']) == 7, 'Q28_SQL8_SDK7_separate')
        check(sdk.PENDING_CASE not in sdk.EXECUTED_CASES and len(sdk.CASES) == 8, 'SDK_shared_spilling_stays_pending')
        check(driver.EXPECTED_CHECKS == (136,472,238,862) and sum(driver.EXPECTED_CHECKS) == 1708,
              'Q28_fixed4_exact1708')
        check(driver._sources().keys() == driver.SOURCE_BINDINGS.keys(), 'Q28_fixed4_loads_all_final_exact_snapshots')
        good = []
        for index, (spec, count) in enumerate(zip(driver.SPECS, driver.EXPECTED_CHECKS)):
            record = {'case_index': index, 'checks_executed': count, 'passed': True}
            for name, value in (('source_refreshes',100), ('union_refreshes',10), ('rule_attempts',100), ('candidate_visits',100)):
                delta = (int(spec['kept']) if name == 'union_refreshes' else
                         0 if name == 'source_refreshes' and spec['kept'] else 1 if spec['kept'] else spec['fanout'])
                record['current_' + name] = value
                record['old_depth_' + name] = value + delta
            good.append(record)
        encoded = lambda records: b'\n'.join(json.dumps(item, sort_keys=True).encode() for item in records)
        check(driver.parse_records(encoded(good), 0) == good, 'Q28_complete_mock_counter_vector_preserved')
        for mutate in (lambda records: records.pop(), lambda records: records.reverse(),
                       lambda records: records[0].update(checks_executed=135),
                       lambda records: records[0].update(old_depth_rule_attempts=109)):
            bad = copy.deepcopy(good)
            mutate(bad)
            try:
                driver.parse_records(encoded(bad), 0)
            except (ValueError, TypeError):
                check(True, 'Q28_malformed_case_or_work_vector_rejected')
            else:
                check(False, 'Q28_malformed_vector_not_rejected')

        native = importlib.import_module('native_verify')
        cap = importlib.import_module('quality_spill_bounds')
        check(len(cap._CELLS) == 82 and len(cap._SOURCE_BINDINGS) == 28, 'spill82_and_dependency28_unchanged')
        vectors['spill82'] = list(cap._CELLS)
        for case, specification in cap._CELLS.items():
            module = importlib.import_module(specification['module'])
            # Match the source snapshot field stamped by the real native owner.
            module._verification_source_sha256 = sha(Path(module.__file__).read_bytes())
            check(module._verification_source_sha256 == cap._SOURCE_BINDINGS[
                'rbo-fixes-20261023/' + specification['module'] + '.py'], 'loaded_cap_generator_identity:' + case)
            metadata = native.module_metadata(module, case, 'new', 24)
            check(cap._digest(metadata) == specification['metadata_sha256'], 'exact_original_spill_metadata:' + case)
            bug = case.split('_', 1)[0].upper()
            for phase in ('baseline', 'fixed'):
                revision = cap._BASE_REVISION if phase == 'baseline' else cap._FIXED_REVISIONS[bug]
                scope = cap._ValidatedSpillScope(phase, revision, (bug,), '/metadata-only/runner', 'a' * 64, 'b' * 64)
                result = cap.bound_quality_spill_metadata(module, case, 'new', 24, metadata,
                                                         scope=scope, runner='/metadata-only/runner')
                settings = result['table_service_overrides']
                check(settings['EnableSpillingNodes'] == 'None' and settings['SpillingServiceConfig'] == cap.LOCAL_SERVICE
                      and settings['EnableQueryServiceSpilling'] is True
                      and settings['EnableSpillingInHashJoinShuffleConnections'] is True,
                      'cap_preserves_channel_policy:' + case + ':' + phase)
                check(result['spill_payload_file_policy']['production_revision'] == revision
                      and result['spill_payload_file_policy']['cap_rejection_counts_as_success'] is False,
                      'cap_exact_revision_and_no_acceptance:' + case + ':' + phase)
        case = 'q28_shared_spilling_control'
        metadata = native.module_metadata(scheduler, case, 'new', 24)
        stale = cap._ValidatedSpillScope('fixed', '567d22ff15c8036cbc9741e0bb29daa6c140f0c1', ('Q28',),
                                        '/metadata-only/runner', 'a' * 64, 'b' * 64)
        try:
            cap.bound_quality_spill_metadata(scheduler, case, 'new', 24, metadata, scope=stale, runner='/metadata-only/runner')
        except ValueError:
            check(True, 'old_Q28_revision_spill_scope_rejected')
        else:
            check(False, 'old_Q28_revision_spill_scope_not_rejected')
        check(sha(Path(native.__file__).read_bytes()) == source_report['native59_sha256'], 'actual_loaded_native59_preserved')
    sys.meta_path.remove(finder)
    report = {'schema': 'quality_idle60_composed_metadata_v1', 'checks_passed': len(checks), 'checks': checks,
        'composition_sha256': sha(REPORT_PATH.read_bytes()), 'rendered_identities_only': renders,
        'vectors': vectors, 'native_or_toolchain_work_executed': False,
        'rendered_SQL_CPP_fixtures_rows_plans_AST_persisted': False,
        'mock_success_is_not_native_acceptance': True,
        'coverage': {'Q01_runtime':42,'Q01_canonical_SQL':652,'Q01_stageA_pilot':30,'Q01_raw_resources':180,
                     'Q14_direct':12,'Q14_SQL_pairs':6,'Q28_original_direct':26,'Q28_additional_direct':4,
                     'Q28_additional_checks':1708,'Q28_SQL':8,'Q28_SDK':7,'spill_cells':82}}
    output = ROOT / 'verification/quality-idle60-composed-metadata-checks-20261028.json'
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'checks_passed':len(checks),'report_sha256':sha(output.read_bytes()),
                      'composition_sha256':sha(REPORT_PATH.read_bytes())}))


if __name__ == '__main__':
    main()
