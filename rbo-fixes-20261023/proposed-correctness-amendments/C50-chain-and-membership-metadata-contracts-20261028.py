"""Metadata-only C50 generator checks; generated payloads remain in RAM."""
import ast
import collections
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import types

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[1]
DORMANT = ROOT / 'proposed-correctness-amendments'
CHECKS = 0

def check(value, category):
    global CHECKS
    if not value:
        raise AssertionError(category)
    CHECKS += 1

def digest(rows):
    return hashlib.sha256('\n'.join(sorted(json.dumps(r, sort_keys=True, separators=(',', ':')) for r in rows)).encode()).hexdigest()

def load(name, source):
    module = types.ModuleType(name)
    module.__file__ = str(ROOT / 'join_key_checks.py')
    exec(compile(source, module.__file__, 'exec'), module.__dict__)
    return module

def bodies(source):
    return {node.name: ast.dump(node, include_attributes=False) for node in ast.parse(source).body if isinstance(node, ast.FunctionDef)}

before = (ROOT / 'join_key_checks.py').read_text()
alias_source = (DORMANT / 'join_key_checks_chain_aliases_20261028.candidate.py').read_text()
member_source = (DORMANT / 'join_key_checks_uncorrelated_membership_20261028.candidate.py').read_text()
check(hashlib.sha256(before.encode()).hexdigest() == '72c2efc2d77ec424f3ee794482728ce73b8d032e5fed88390d339f6d7e3310fe', 'live generator baseline binding')
with tempfile.TemporaryDirectory(prefix='c50-source-contracts-') as tmp:
    target = pathlib.Path(tmp) / 'join_key_checks.py'
    target.write_text(before)
    for patch, expected in (('C50-chain-projection-aliases-20261028.patch', alias_source), ('C50-uncorrelated-membership-controls-20261028.patch', member_source)):
        result = subprocess.run(['git', 'apply', str(DORMANT / patch)], cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(result.returncode == 0, 'source patch clean application')
        check(target.read_text() == expected, 'exact candidate after image')

original, alias, member = [load(name, source) for name, source in [('original', before), ('alias', alias_source), ('member', member_source)]]
check({k for k in bodies(before) if bodies(before)[k] != bodies(alias_source)[k]} == {'_query'}, 'alias repair only query builder')
for name in ('_rows', 'schema_and_seeds', 'expected_rows', 'semantic_oracle'):
    check(bodies(before)[name] == bodies(member_source)[name], 'independent oracle and fixture unchanged')

records_path = ROOT / 'verification/fixed-C50-946575513d23.jsonl'
records = [json.loads(line) for line in records_path.read_text().splitlines()]
check(len(records) == 62, 'exact completed C50 evidence count')
check(sum(r.get('semantic_oracle_pass') is True for r in records) == 50, 'original passing evidence count')
check(sum(r.get('semantic_oracle_pass') is False for r in records) == 12, 'original failing evidence preserved')
evidence = []
for label in ('c50_join_chain', 'c50_join_chain_se'):
    cells = [r for r in records if r.get('case') == label]
    check(len(cells) == 2, 'chain repetitions')
    expected = original.expected_rows(label, cells[0]['rows'])
    qualified = [{'q.' + k if k in ('li', 'ri') else k: v for k, v in row.items()} for row in expected]
    exact = all(digest(expected) == r.get('unordered_result_sha256') for r in cells)
    renamed = all(digest(qualified) == r.get('unordered_result_sha256') for r in cells)
    check(not exact and renamed, 'chain complete bag digest differs only in projection names')
    check(all(len(expected) == r.get('row_count') for r in cells), 'chain row counts match')
    evidence.append({'case': label, 'records': len(cells), 'qualified_names_only_digest_match': renamed, 'native_rerun_still_required': True})
for label in ('c50_left_semi', 'c50_left_anti', 'c50_right_semi', 'c50_right_anti'):
    cells = [r for r in records if r.get('case') == label]
    controls = [r for r in records if r.get('case') == label + '_same_type_control']
    check(len(cells) == len(controls) == 2, 'mixed correlated and homogeneous controls have two repetitions')
    right = 'right' in label
    prediction = [] if 'semi' in label else [{('ri' if right else 'li'): i} for i, _ in original._rows(cells[0]['rows'], label, 'right' if right else 'left')]
    predicted = all(digest(prediction) == r.get('unordered_result_sha256') for r in cells)
    check(predicted, 'all correlated failure bags match typed-domain mismatch prediction')
    check(all(r['semantic_oracle_pass'] is True for r in controls), 'same-type correlated controls pass')
    evidence.append({'case': label, 'records': len(cells), 'existing_C06_source_cause': True, 'predicted_failure_bag_digest_match': predicted, 'C06_plus_C50_native_integration_still_required': True})

for n in (8, 24, 48):
    old = original.recipes(n)
    repaired = alias.recipes(n)
    extended = member.recipes(n)
    check(old.keys() == repaired.keys(), 'alias repair preserves case set')
    changed = {label for label in old if old[label] != repaired[label]}
    check(changed == {'c50_join_chain', 'c50_join_chain_se'}, 'alias repair affects only chain recipes')
    check(all(extended[label] == repaired[label] for label in repaired), 'membership controls preserve every prior recipe')
    extra = set(extended) - set(repaired)
    check(len(extra) == 8, 'four mixed and four same-type membership controls')
    for label in sorted(extra):
        query = extended[label]
        check('EXISTS' not in query.upper(), 'new controls avoid existence decorrelation')
        check(query.upper().count('COALESCE(') == 2, 'both membership inputs become required')
        check(('NOT IN' in query.upper()) == ('anti' in label), 'membership polarity retained')
        check(' IN ' in query.upper(), 'membership construction')
        expected = member.expected_rows(label, n)
        check(member.semantic_oracle(label, expected, n), 'unchanged bag oracle accepts correct rows')
        if expected:
            check(not member.semantic_oracle(label, expected[:-1], n), 'oracle rejects missing row')
            check(not member.semantic_oracle(label, expected + expected[:1], n), 'oracle rejects extra duplicate')
        else:
            check(not member.semantic_oracle(label, [{'li' if 'left' in label else 'ri': 1}], n), 'empty anti result rejects a preserved row')
        check(not member.semantic_oracle(label, [{'wrong_output': 0}], n), 'oracle rejects wrong schema')
        check(all(key is not None for side in ('left', 'right') for _, key in member._rows(n, label, side)), 'fixture normalization does not replace any NULL')
        check(member.requirements(label)['node_count'] == 4, 'membership controls retain distributed runner')
        check(member.requirements(label)['table_service_config']['UseBlockHashJoin'] is True, 'new controls scoped to block path')
        check(not member.requirements(label)['table_service_config']['DefaultEnableShuffleElimination'], 'new controls isolate disabled shuffle elimination')
        if 'same_type_control' not in label:
            check(member.paired_controls()[label] == [label + '_same_type_control'], 'same-type partner bound')
        # Trace generated builder arguments in memory: the membership subquery
        # has only the inner alias; no outer capture reaches its expression.
        old_select = member.g.select
        calls = []
        def observe(fields, source=None, **kwargs):
            calls.append((fields, source, kwargs))
            return old_select(fields, source, **kwargs)
        member.g.select = observe
        try:
            member._query(label)
        finally:
            member.g.select = old_select
        subquery_calls = [entry for entry in calls if entry[0] in (['a.k'], ['b.k'])]
        check(len(subquery_calls) == 1, 'one scalar RHS subquery')
        fields, source, kwargs = subquery_calls[0]
        other = 'a.' if fields == ['b.k'] else 'b.'
        check(other not in source and not kwargs, 'subquery has no outer reference or correlated predicate')

summary = {'kind': 'metadata_only_source_contracts', 'assertions': CHECKS, 'original_native_records': 62,
           'original_native_passes': 50, 'original_native_failures': 12,
           'new_membership_profiles': 8, 'new_native_samples': 0,
           'native_execution': False, 'build_or_toolchain_execution': False,
           'live_source_changes': False, 'rendered_payload_persistence': False,
           'evidence_sha256': hashlib.sha256(records_path.read_bytes()).hexdigest(), 'observations': evidence}
output = ROOT / 'verification/C50-chain-and-membership-metadata-contracts-20261028.json'
output.write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({'assertions': CHECKS, 'metadata_pass': True, 'new_native_samples': 0, 'report_sha256': hashlib.sha256(output.read_bytes()).hexdigest()}))
