"""Source/metadata checks only; no native/toolchain subprocesses or generated files."""
import ast
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from anonymous_cpp_metadata import MetadataError


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path):
    m = ModuleType(path.stem)
    m.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), m.__dict__)
    return m


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    def reject(fn, label):
        try:
            fn()
        except (ValueError, TypeError, KeyError, OSError, MetadataError):
            check(True, label)
        else:
            check(False, label)
    with tempfile.TemporaryDirectory(prefix='c31-pipeline-metadata-') as td:
        temp = Path(td)
        names = ('c31_identity_cpp_generator.py', 'c31_shared_pipeline_cpp_generator.py',
                 'c31_combined_cpp_generator.py', 'correctness_cpp_contract.py',
                 'correctness_cpp_profiles.json', 'anonymous_cpp_verify.py')
        for name in names:
            raw = (HERE/name).read_bytes()
            (temp/name).write_bytes(raw)
            if name.endswith('.py'):
                compile(raw, str(temp/name), 'exec')
                check(True, 'python_source_valid_' + name)
        for name in ('c47_combined_cpp_generator.py', 'c47_lookup_copy_contract_generator.py',
                     'c47_covering_import_contract_generator.py'):
            (temp/name).write_bytes((ROOT/name).read_bytes())
        (temp/'cases').mkdir()
        for bug in ('C31', 'C47'):
            (temp/'cases'/(bug+'.json')).write_bytes((ROOT/'cases'/(bug+'.json')).read_bytes())
        old = load(temp/'c31_identity_cpp_generator.py')
        new = load(temp/'c31_shared_pipeline_cpp_generator.py')
        combined = load(temp/'c31_combined_cpp_generator.py')
        contract = load(temp/'correctness_cpp_contract.py')
        profile_doc = json.loads((temp/'correctness_cpp_profiles.json').read_bytes())
        profile = profile_doc['profiles']['C31']
        revision = profile['revision']
        check((temp/'c31_identity_cpp_generator.py').read_bytes() == (ROOT/'c31_identity_cpp_generator.py').read_bytes(), 'original33_source_byte_identical')
        records = json.loads((ROOT/'verification/direct-run-C31-1793147369208959585.json').read_bytes())['cases']
        check(len(records) == 33 and sum(x['checks_executed'] for x in records) == 1416 and all(x['passed'] for x in records), 'actual_original33_1416_bound')
        specs = combined.specs()
        check(len(specs) == 37 and len(combined.cases()) == 37, 'combined37')
        check([x['case'] for x in specs[:33]] == old.cases(), 'old33_order_unchanged')
        check([x['assertion_count'] for x in specs[:33]] == [x['checks_executed'] for x in records], 'old33_exact_assertion_vector')
        check([x['case'] for x in specs[33:]] == new.cases(), 'new4_order')
        check([x['assertion_count'] for x in specs[33:]] == [8, 8, 8, 2], 'new4_checkpoint_vector')
        check(sum(x['assertion_count'] for x in specs) == 1442, 'combined_reported_checkpoints1442')
        rendered = combined.render_cpp_suite()
        check(rendered.count('int main()') == 1, 'single_driver')
        check(rendered.rfind('#include ') < rendered.find('namespace C31_identity'), 'headers_at_file_scope')
        for binding, module, source in combined._components():
            prefix = source.split(binding['main_boundary'])[0]
            body = ''.join(x for x in prefix.splitlines(True) if not x.startswith('#include <'))
            check('namespace C31_' + binding['vector'] + ' {\n' + body + '}\n' in rendered, 'component_bodies_byte_identical_' + binding['vector'])
        source, count, generator_hash = contract.render_suite('C31', revision)
        check(source.decode() == rendered and count == 37 and generator_hash == sha((temp/'c31_combined_cpp_generator.py').read_bytes()), 'profile_render_exact')
        check(contract.vector_evidence('C31', revision)['direct_contract_vectors'] == combined.vector_manifest(), 'vector_evidence_required')
        good = [{'case_index': x['case_index'], 'checks_executed': x['assertion_count'], 'passed': True} for x in specs]
        encode = lambda rows: b'\n'.join(json.dumps(x).encode() for x in rows)
        check(contract.parse_verdicts(encode(good), 37, 0, 'C31') == good, 'complete_verdict_vector_admitted')
        for i, row in enumerate(good):
            for delta in (-1, 1):
                altered = copy.deepcopy(good)
                altered[i]['checks_executed'] += delta
                reject(lambda: contract.parse_verdicts(encode(altered), 37, 0, 'C31'), 'exact_count_reject_' + str(i) + '_' + str(delta))
            changed = copy.deepcopy(good)
            changed[i]['passed'] = False
            check(contract.parse_verdicts(encode(changed), 37, 1, 'C31')[i]['passed'] is False, 'failure_preserved_' + str(i))
        reject(lambda: contract.parse_verdicts(encode(good[:33]), 37, 0, 'C31'), 'old33_alone_insufficient')
        reject(lambda: contract.parse_verdicts(encode(good[33:]), 37, 0, 'C31'), 'new4_alone_insufficient')
        reject(lambda: contract.parse_verdicts(encode(good[:-1]+[good[0]]), 37, 0, 'C31'), 'duplicate_case_rejected')
        reject(lambda: contract.parse_verdicts(encode(good), 37, 1, 'C31'), 'exit_disagreement_rejected')
        for component in ('c31_identity_cpp_generator.py', 'c31_shared_pipeline_cpp_generator.py'):
            path = temp/component
            raw = path.read_bytes()
            path.write_bytes(raw+b'\n')
            reject(combined.render_cpp_suite, 'component_hash_change_rejected_' + component)
            path.write_bytes(raw)
        raw_profile = (temp/'correctness_cpp_profiles.json').read_bytes()
        for field, value in [('case_count', 33), ('generated_source_sha256', '0'*64), ('generator_sha256', '0'*64)]:
            changed = copy.deepcopy(profile_doc)
            changed['profiles']['C31'][field] = value
            (temp/'correctness_cpp_profiles.json').write_text(json.dumps(changed))
            reject(lambda: contract.render_suite('C31', revision), 'profile_change_rejected_' + field)
            (temp/'correctness_cpp_profiles.json').write_bytes(raw_profile)
        for index in (0, 32, 33, 36):
            changed = copy.deepcopy(profile_doc)
            changed['profiles']['C31']['case_assertions'][index]['assertion_count'] += 1
            (temp/'correctness_cpp_profiles.json').write_text(json.dumps(changed))
            reject(lambda: contract.render_suite('C31', revision), 'case_vector_change_rejected_' + str(index))
            (temp/'correctness_cpp_profiles.json').write_bytes(raw_profile)
        original_profiles = json.loads((ROOT/'correctness_cpp_profiles.json').read_bytes())
        check(profile_doc['profiles']['C47'] == original_profiles['profiles']['C47'], 'C47_profile_unchanged')
        c47 = profile_doc['profiles']['C47']
        other, other_count, _ = contract.render_suite('C47', c47['revision'])
        check(sha(other) == c47['generated_source_sha256'] and other_count == 51, 'C47_render_unchanged')
        before = ast.parse((ROOT/'anonymous_cpp_verify.py').read_bytes())
        after = ast.parse((HERE/'anonymous_cpp_verify.py').read_bytes())
        a = [ast.dump(x, include_attributes=False) for x in before.body if isinstance(x, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]
        b = [ast.dump(x, include_attributes=False) for x in after.body if isinstance(x, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]
        check(a == b, 'all_driver_functions_and_guards_unchanged')
        before_c = ast.parse((ROOT/'correctness_cpp_contract.py').read_bytes())
        after_c = ast.parse((HERE/'correctness_cpp_contract.py').read_bytes())
        get = lambda tree, name: ast.dump(next(x for x in tree.body if isinstance(x, ast.FunctionDef) and x.name == name), include_attributes=False)
        check(get(before_c, 'validate_fixed_local_contracts') == get(after_c, 'validate_fixed_local_contracts'), 'provenance_validation_body_unchanged')
        cpp = new._prelude()
        for token in ('CreateExtCallableTypeAnnotationTransformer(F.TypeCtx)', 'CreateTypeAnnotationTransformer(Callable, F.TypeCtx)',
                      'table.Load(F.ExprCtx)', 'ComputeRequiredProps(*Root', 'TGlobalPruningStage prune',
                      'TGlobalInliningStage inlineCopies', 'TAssignStagesStage stage', 'TPropagateAggregateThroughStageRule',
                      'ComputePlanLiveness(*f.Root, ELivenessMode::Global)', 'NTests::AssertIdInvariants',
                      'TPhysicalAggregationBuilder(producerPartial', 'TPhysicalAggregationBuilder(producer,',
                      'NPhysicalConvertionUtils::BuildSwitch', 'NPhysicalConvertionUtils::TransformStageOutput',
                      'GetReplicateOutputIndex(*s.Secondary)', 'GetReplicateOutputIndex(partial)',
                      'member->Child(0) == row->Child(0)->Child(0)', 'keys->Child(index + 1) == keys->Child(0)->Child(inputPositions.at(name))',
                      'results.at(name) == resultMap->Child(0)->Child(index)', 'source != f.Count',
                      'op == &final', 'intermediate == partial.GetAggregationTraits().Keys()',
                      'Join filter reconstruction requires stable copied values', 'f.OriginalProducer->Type == producerType'):
            check(token in cpp, 'actual_contract_source_' + sha(token.encode())[:12])
        for token in ('f.SetType(', 'F.SetType(', 'TypeAnnTransformer.Transform(', 'ParsePhysical', 'KqpExprToPrettyString', 'PrintPlan', 'system('):
            check(token not in cpp, 'forbidden_shortcut_absent_' + sha(token.encode())[:12])
        run = cpp.split('static void Run(bool grouped, bool some, bool shared) {', 1)[1]
        prefix, rest = run.split('if (!shared) {', 1)
        negative, positive = rest.split('TShape s{};', 1)
        check(prefix.count('Checkpoint(') == 1 and negative.count('Checkpoint(') == 1 and positive.count('Checkpoint(') == 7, 'checkpoint_counts_from_actual_control_flow')
        check('Lower(f, s, false)' in positive and 'Lower(f, s, true)' in positive, 'both_builder_modes')
        # Basic operator-node adversaries are exercised by the production-native
        # checker itself after review; Python does not pretend to simulate C++.
        for invalid in ([], [new.cases()[0]]*2, ['unknown']):
            reject(lambda: new.render_cpp_suite(invalid), 'invalid_component_selection_' + str(len(checks)))
        check(not contract.validate_fixed_local_contracts({}, 'C31', revision), 'no_mock_or_partial_provenance_admission')
    report = {'schema':'c31-shared-pipeline-source-metadata-v1', 'checks_passed':len(checks),
              'checks':checks, 'native_or_compiler_run':False, 'rendered_cpp_saved':False,
              'old_case_count':33, 'old_actual_checks':1416, 'new_case_count':4,
              'new_reported_checkpoint_counts':[8,8,8,2], 'combined_reported_checks':1442,
              'checkpoint_definition':'One checkpoint succeeds only if every internal production call and Require predicate succeeds.',
              'full_final_sql_lineage_proven':False}
    path = ROOT/'verification/C31-shared-pipeline-metadata-contracts-20261028.json'
    path.write_text(json.dumps(report, indent=2, sort_keys=True)+'\n')
    print(json.dumps({'checks_passed':len(checks), 'report':str(path), 'sha256':sha(path.read_bytes())}))

if __name__ == '__main__':
    main()
