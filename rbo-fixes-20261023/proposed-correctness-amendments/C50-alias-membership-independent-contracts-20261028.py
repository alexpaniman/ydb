"""Independent source/digest review; generated inputs and result models stay in RAM."""
import ast
import contextlib
import hashlib
import io
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
DORMANT = ROOT / 'proposed-correctness-amendments'
BASE = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
FIXED = '946575513d233534e8f4fa4168f5dc9379ffc982'
C06 = '446592a476b92437df5b2579e1027cfb313be1d9'
CHECKS = 0


def check(value):
    global CHECKS
    if not value:
        raise AssertionError('Independent metadata contract failed')
    CHECKS += 1


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(rows):
    return sha('\n'.join(sorted(json.dumps(row, sort_keys=True, separators=(',', ':')) for row in rows)).encode())


def source(revision, path):
    return subprocess.check_output(['git', 'show', revision + ':' + path], cwd=ROOT.parent)


def load(name, payload):
    module = types.ModuleType(name)
    module.__file__ = str(ROOT / 'join_key_checks.py')
    exec(compile(payload, module.__file__, 'exec'), module.__dict__)
    return module


def functions(payload):
    return {node.name: ast.dump(node, include_attributes=False)
            for node in ast.parse(payload).body if isinstance(node, ast.FunctionDef)}


author_report_path = ROOT / 'verification/C50-failure-diagnosis-and-generator-source-preparation-20261028.json'
author_report = json.loads(author_report_path.read_bytes())
check(sha(author_report_path.read_bytes()) == 'cd41392a418109dcbdd5f7733495117cd63d8642ebfad67efd47c46b130e5d4e')
for artifact in author_report['artifacts']:
    check(sha((ROOT / artifact['path']).read_bytes()) == artifact['sha256'])
for binding in author_report['source_bindings']:
    check(sha(source(binding['revision'], binding['path'])) == binding['sha256'])

before = (ROOT / 'join_key_checks.py').read_bytes()
alias = (DORMANT / 'join_key_checks_chain_aliases_20261028.candidate.py').read_bytes()
member = (DORMANT / 'join_key_checks_uncorrelated_membership_20261028.candidate.py').read_bytes()
check(sha(before) == author_report['live_generator_before_sha256'])
check(alias == before.replace(b"['q.li', 'q.ri', g.alias('c.id', 'ti')]",
                             b"[g.alias('q.li', 'li'), g.alias('q.ri', 'ri'), g.alias('c.id', 'ti')]"))
with tempfile.TemporaryDirectory(prefix='c50-independent-source-') as directory:
    target = Path(directory) / 'join_key_checks.py'
    target.write_bytes(before)
    for name, expected in [('C50-chain-projection-aliases-20261028.patch', alias),
                           ('C50-uncorrelated-membership-controls-20261028.patch', member)]:
        result = subprocess.run(['git', 'apply', str(DORMANT / name)], cwd=directory,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(result.returncode == 0)
        check(target.read_bytes() == expected)

original, repaired, extended = [load(name, data) for name, data in
                                [('original', before), ('repaired', alias), ('extended', member)]]
before_functions, alias_functions, member_functions = map(functions, (before, alias, member))
check({name for name in before_functions if before_functions[name] != alias_functions[name]} == {'_query'})
check({name for name in alias_functions if alias_functions[name] != member_functions[name]} ==
      {'_input', '_query', 'recipes', 'requirements', 'paired_controls'})
for name in ('_rows', 'schema_and_seeds', 'expected_rows', 'semantic_oracle'):
    check(before_functions[name] == alias_functions[name] == member_functions[name])

for n in (8, 13, 24, 48):
    old_recipes, alias_recipes, member_recipes = [m.recipes(n) for m in (original, repaired, extended)]
    check(len(old_recipes) == len(alias_recipes) == 31)
    check(len(member_recipes) == 39)
    check({name for name in old_recipes if old_recipes[name] != alias_recipes[name]} ==
          {'c50_join_chain', 'c50_join_chain_se'})
    for name in old_recipes:
        check(alias_recipes[name] == member_recipes[name])
        check(original.schema_and_seeds(n, name) == extended.schema_and_seeds(n, name))
        check(original.expected_rows(name, n) == extended.expected_rows(name, n))
        for mode in ('new', 'legacy'):
            check(original.requirements(name, mode) == extended.requirements(name, mode))
    extras = set(member_recipes) - set(old_recipes)
    for name in extras:
        corresponding = name.replace('_uncorrelated', '')
        check(extended.schema_and_seeds(n, name) == original.schema_and_seeds(n, corresponding))
        check(extended.expected_rows(name, n) == original.expected_rows(corresponding, n))
        check(all(value is not None for side in ('left', 'right') for _, value in extended._rows(n, name, side)))
        values = extended.expected_rows(name, n)
        check(extended.semantic_oracle(name, values, n))
        check(extended.semantic_oracle(name, list(reversed(values)), n))
        if values:
            check(not extended.semantic_oracle(name, values[1:], n))
            check(not extended.semantic_oracle(name, values + values[:1], n))
            wrong = [dict(row) for row in values]
            field = next(iter(wrong[0]))
            wrong[0][field] += n + 1
            check(not extended.semantic_oracle(name, wrong, n))
        else:
            check(not extended.semantic_oracle(name, [{'ri' if 'right' in name else 'li': n + 1}], n))
        check(not extended.semantic_oracle(name, [{'unexpected_column': 0}], n))
        requirements = extended.requirements(name)
        check(requirements['node_count'] == 4 and requirements['repetitions'] == 2)
        check(requirements['table_service_config']['UseBlockHashJoin'] is True)
        check(requirements['table_service_config']['DefaultEnableShuffleElimination'] is False)
        if 'same_type_control' not in name:
            check(extended.paired_controls()[name] == [name + '_same_type_control'])

guarded_path = ROOT / 'verification/guarded-fixed-C50-946575513d23-1793145911575205063.jsonl'
guarded_data = guarded_path.read_bytes()
check(sha(guarded_data) == 'ee458a3f9112bc998787db65e5a0816b265d1a979a7ff8fb5b0dd9448033316f')
records = [json.loads(line) for line in guarded_data.splitlines()]
check(len(records) == 62)
check(sum(record['semantic_oracle_pass'] for record in records) == 50)
chain_matches = mixed_matches = same_type_matches = 0
for record in records:
    name, n = record['case'], record['rows']
    expected = original.expected_rows(name, n)
    direct = digest(expected) == record['unordered_result_sha256']
    check(direct == record['semantic_oracle_pass'])
    check(record['binary_provenance_verified'] and record['native_batch_guard_pass'])
    check(record['correctness_physical_ast_required'] is False and record['correctness_query_plan_required'] is False)
    if 'join_chain' in name:
        qualified = [{('q.' + key if key in ('li', 'ri') else key): value for key, value in row.items()}
                     for row in expected]
        check(not direct and digest(qualified) == record['unordered_result_sha256'])
        check(len(expected) == record['row_count'])
        chain_matches += 1
    elif '_semi' in name or '_anti' in name:
        if 'same_type' in name:
            check(direct)
            same_type_matches += 1
        else:
            side = 'left' if '_left_' in name else 'right'
            prediction = [] if '_semi' in name else [
                {'li' if side == 'left' else 'ri': identifier}
                for identifier, _ in original._rows(n, name, side)]
            check(not direct and digest(prediction) == record['unordered_result_sha256'])
            mixed_matches += 1
check((chain_matches, mixed_matches, same_type_matches) == (4, 8, 8))

baseline_path = ROOT / 'verification/baseline-C50-e8700d379b3b.jsonl'
baseline_data = baseline_path.read_bytes()
baseline = [json.loads(line) for line in baseline_data.splitlines()]
historical = [record for record in baseline if record.get('mode') == 'new'
              and record.get('generator_sha256') == sha(before)
              and ('_semi' in record.get('case', '') or '_anti' in record.get('case', ''))]
check(len(historical) == 8)
for record in historical:
    counterpart = next(item for item in records if item['case'] == record['case']
                       and item['repetition'] == record['repetition'])
    for key in ('query_sha256', 'schema_sha256', 'fixture_sha256', 'settings_sha256',
                'rows', 'generator_sha256', 'unordered_result_sha256', 'row_count', 'semantic_oracle_pass'):
        check(record.get(key) == counterpart.get(key))
    check('binary_provenance_verified' not in record and 'node_count' not in record)

dependent_path = 'ydb/core/kqp/opt/rbo/rules/decorrelation/dependent_join_pushdown.cpp'
dependent = source(FIXED, dependent_path).decode()
check(source(BASE, dependent_path) == dependent.encode())
check('!IsSameAnnotation(*capture.Type, *sourceType)' not in dependent)
check('!IsSameAnnotation(*capture.Type, *sourceType)' in source(C06, dependent_path).decode())
check('ERuleProperties::RequireTypes' in source(C06, 'ydb/core/kqp/opt/rbo/kqp_rbo_rules.h').decode().split(
    'class TEliminateDependentJoinDomainRule:', 1)[1].split('virtual bool', 1)[0])
transformer = source(FIXED, 'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp').decode()
check(transformer.index('std::make_unique<TEliminateDependentJoinDomainRule>') < transformer.index('std::make_unique<TAssignStagesStage>'))
inline = source(FIXED, 'ydb/core/kqp/opt/rbo/rules/inline_simple_in_exists_subplan.cpp').decode()
check('const bool useDependentJoin = HasFreeCorrelation(subplan, subplanEntry->DependentIUs);' in inline)
check('join = MakeIntrusive<TOpJoin>(leftJoinInput, subplan, input->Pos, joinKind, tupleJoinKeys);' in inline)
check('auto joinKind = negated ? "LeftOnly" : "LeftSemi";' in inline)
check('optional bool EnableBlockHashJoinEqualNulls = 149 [default = false' in
      source(FIXED, 'ydb/core/protos/table_service_config.proto').decode())

# Rerun the author script while capturing its report in RAM; frozen artifacts stay unchanged.
author_metadata_path = ROOT / 'verification/C50-chain-and-membership-metadata-contracts-20261028.json'
author_metadata_before = author_metadata_path.read_bytes()
captured = []
write_text = Path.write_text


def capture_report(path, content, *args, **kwargs):
    if path == author_metadata_path:
        captured.append(content.encode())
        return len(content)
    return write_text(path, content, *args, **kwargs)


with patch.object(Path, 'write_text', capture_report), contextlib.redirect_stdout(io.StringIO()):
    author_result = runpy.run_path(str(DORMANT / 'C50-chain-and-membership-metadata-contracts-20261028.py'))
check(captured == [author_metadata_before])
check(author_metadata_path.read_bytes() == author_metadata_before)
check(author_result['CHECKS'] == 387)
check((ROOT / 'join_key_checks.py').read_bytes() == before)
check(guarded_path.read_bytes() == guarded_data)
check(baseline_path.read_bytes() == baseline_data)

report = {
    'kind': 'independent_source_metadata_and_existing_digest_review',
    'source_approved': True, 'native_execution': False, 'build_or_toolchain_execution': False,
    'live_edits': False, 'rendered_payload_persistence': False,
    'independent_assertions': CHECKS, 'author_assertions_rerun': 387,
    'source_preparation_sha256': sha(author_report_path.read_bytes()),
    'patches': [item for item in author_report['artifacts'] if item['path'].endswith('.patch')],
    'guarded_evidence_sha256': sha(guarded_data), 'historical_baseline_sha256': sha(baseline_data),
    'original_records_preserved': {'total': 62, 'passing': 50, 'failing': 12},
    'findings': [
        {'classification': 'existing_independent_C06_correctness_bug', 'records': 8,
         'description': 'Domain elimination substitutes a differently typed equality input for a capture; later typed domain serialization can lose matches.',
         'evidence': 'Eight exact no-domain-match bag digests; eight same-type controls pass; typed substitution and encoding path source-checked.',
         'historical_support': 'Four matching mixed failures and four same-type passes with identical generator/workload/result hashes.',
         'historical_limit': 'Historical baseline lacks binary provenance and recorded node count; it is not a qualified paired baseline.',
         'remaining': 'Independent C06 branch retained; C06 plus C50 integration remains pending.'},
        {'classification': 'verification_generator_projection_names', 'records': 4,
         'description': 'Join-chain projections expose qualified names while the independent bag oracle expects stable aliases.',
         'evidence': 'Complete expected bag hashes match all four records after column-key qualification only; counts and values unchanged.',
         'repair': 'One-line explicit projection aliases; oracle, fixtures, duplicates, settings, and all other queries unchanged.',
         'remaining': 'Actual repaired-chain native rerun is required; original failures remain false.'}
    ],
    'new_controls': {'count': 8, 'shape': 'required uncorrelated membership in four semantic orientations with same-type partners',
                     'no_outer_capture_source_checked': True, 'actual_typed_gather_reachability_pending': True,
                     'physical_right_semi_node_claim': False, 'nullable_not_in_claim': False,
                     'row_path_claim': False, 'shuffle_elimination_claim': False},
    'qualification': {'full_C50_acceptance': False, 'semantic_subset_evidence_only': True,
                      'physical_gather_witness_from_current_batch': False,
                      'do_not_stack_C06_into_standalone_C50': True,
                      'no_original_failure_relabeling': True},
}
output = ROOT / 'verification/C50-alias-membership-independent-review-20261028.json'
output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'independent_assertions': CHECKS, 'author_assertions_rerun': 387,
                  'report_sha256': sha(output.read_bytes()), 'source_approved': True,
                  'full_C50_acceptance': False}))
