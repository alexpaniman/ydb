"""Synthetic metadata only; no real runner, query, or fixture content."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
path = Path(__file__).with_name('historical_rerun_vector_check_20261028.py')
spec = importlib.util.spec_from_file_location('vector_check', path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
MANIFEST = checker.document(checker.MANIFEST_PATH.read_bytes())
assert checker.sha(checker.MANIFEST_PATH.read_bytes()) == checker.MANIFEST_SHA256
COUNT = 0

def check(value, label):
    global COUNT
    if not value:
        raise AssertionError(label)
    COUNT += 1

def encode(value):
    return json.dumps(value, sort_keys=True).encode()

def scenario(item):
    bug, revision = item['bug'], item['revision']
    stamp = str(MANIFEST['prepared_unix_ns'] + 1000)
    receipt_path = ROOT / 'verification/build-receipts' / ('metadata-' + bug + '.json')
    guard_path = ROOT / 'verification' / ('native-memory-fixed-' + bug + '-' + revision[:12] + '-' + stamp + '.json')
    slice_path = ROOT / 'verification' / ('guarded-fixed-' + bug + '-' + revision[:12] + '-' + stamp + '.jsonl')
    build_path = ROOT / 'verification' / ('metadata-fullsuite-' + bug + '.json')
    binary = {'sha256': 'a' * 64, 'size_bytes': 200, 'ctime_ns': 2, 'mtime_ns': 1,
              'inode': 1, 'device': 1, 'resolved_path': '/metadata-only/executable'}
    target = 'ydb/tests/tools/kqprun' + (' ydb/core/kqp/ut/rbo -tA' if bug == 'C41' else '')
    build = {'bug': bug, 'revision': revision, 'exit_code': 0, 'target': target,
             'elapsed_seconds': 1.5, 'diagnostic_sha256': 'b' * 64, 'build_receipt': str(receipt_path),
             'memory': {'watchdog_stopped': False, 'explicit_build_workers': 8,
                        'watchdog_min_host_available_gib': 36, 'watchdog_max_scope_usage_gib': 52}}
    receipt = {'worktree': MANIFEST['expected_worktree'], 'schema_version': 'successful_runner_build_v1', 'revision': revision, 'binary': binary, 'recorded_unix_ns': MANIFEST['prepared_unix_ns'] + 1,
               'build': {k:build[k] for k in ('exit_code', 'target', 'elapsed_seconds', 'diagnostic_sha256')}}
    guard = {'exit_code': 0, 'cleanup_complete': True, 'watchdog_stopped': False,
             'stdout_overflow': False, 'max_scope_gib': 24, 'min_host_available_gib': 36,
             'minimum_host_available_bytes': 40 * checker.GIB}
    artifacts = {receipt_path: encode(receipt), guard_path: encode(guard), build_path: encode(build)}
    records = []
    for cell in item['cells']:
        for repetition in (cell['repetitions'] if cell['repetitions'] is not None else [None]):
            record = {'bug': bug, 'phase': 'fixed', 'revision': revision, 'rows': 24, 'case': cell['case'],
                'mode': cell['mode'], 'exit_code': 0, 'semantic_oracle_pass': True, 'row_count': 0, 'result_sha256': 'a' * 64,
                'native_batch_guard_pass': True, 'binary_provenance_required': True, 'binary_provenance_verified': True,
                'binary_provenance_version': 'successful_runner_build_v1',
                'binary_provenance_comparison_version': 'receipt_ctime_only_revalidation_v1',
                'build_receipt_path': str(receipt_path), 'build_receipt_sha256': checker.sha(artifacts[receipt_path]),
                'binary_provenance': dict(binary), 'build_receipt_ctime_changed': False,
                'native_batch_memory_evidence': str(guard_path), 'native_batch_memory_sha256': checker.sha(artifacts[guard_path])}
            if cell['record_kind'] == 'generator_snapshot':
                record.update(repetition=repetition, generator_hash_source='loaded_source_snapshot',
                    **{k:cell[k] for k in ('generator_module', 'generator_sha256', 'generator_version', 'node_count', 'query_syntax')},
                    **cell['bindings'], **cell['required_record_fields'], **cell['required_oracle_fields'])
                record['optimizer_mode_config_verified'] = True
                record['optimizer_mode_configuration'] = {'version': 'requested_optimizer_configuration_v1',
                    'requested_mode': 'new', 'query_syntax': 'yql', 'enable_new_rbo': True,
                    'enable_fallback': cell['fallback_requested'], 'fallback_requested': cell['fallback_requested'],
                    'yql_select': 'force', 'settings_sha256': record['settings_sha256']}
            if cell['expected_outcome'] == 'guard':
                record.update(exit_code=1, failure_stage='target', correctness_guard_observed=True, semantic_oracle_pass=False)
            elif cell['expected_outcome'] == 'target_failure':
                record.update(exit_code=1, failure_stage='target', expected_failure_observed=True)
            if bug == 'C02' and cell['case'].startswith('c02_volatile_'):
                record['nonvacuous_group_observed'] = True
            if cell['case'].startswith(('c18_retained_two_empty_sets_', 'c18_two_empty_sets_')):
                record.update(metamorphic_pair='c18_empty' if cell['case'].endswith('_empty') else 'c18_nonempty',
                    metamorphic_role='retained' if cell['case'].startswith('c18_retained_') else 'pruned',
                    metamorphic_projection_sha256='c' * 64)
            records.append(record)
    def refresh(values):
        data = b'\n'.join(encode(row) for row in values) + b'\n'
        artifacts[slice_path] = data
        return data
    def read(file):
        return artifacts[file] if file in artifacts else file.read_bytes()
    return records, artifacts, slice_path, receipt_path, guard_path, build_path, refresh, read

for item in MANIFEST['items']:
    records, artifacts, slice_path, receipt_path, guard_path, build_path, refresh, read = scenario(item)
    result = checker.audit(MANIFEST, item['bug'], slice_path, refresh(records), read,
                           fullsuite_build_path=build_path if item['bug']=='C41' else None)
    check(result['exact_vector_passed'] and not result['automatic_full_finding_promotion'], 'valid complete metadata vector remains scoped')
    check(result['checked_records'] == item['expected_record_count'], 'exact expected count')
    check(result['remaining_qualifications'] == item['remaining_qualifications'], 'qualifications preserved')
    for mutation in ('missing', 'duplicate', 'extra', 'bad_revision', 'wrong_mode', 'wrong_rows', 'bool_exit',
                     'missing_guard', 'bad_binary', 'bad_generator', 'missing_plan_flag', 'bad_query_hash',
                     'bad_repetition', 'bool_nodes', 'false_oracle'):
        altered = copy.deepcopy(records)
        if mutation=='missing': altered.pop()
        elif mutation=='duplicate': altered[-1] = copy.deepcopy(altered[0])
        elif mutation=='extra': altered.append(copy.deepcopy(altered[0]))
        elif mutation=='bad_revision': altered[0]['revision'] = '0' * 40
        elif mutation=='wrong_mode': altered[0]['mode'] = 'legacy'
        elif mutation=='wrong_rows': altered[0]['rows'] = 23
        elif mutation=='bool_exit': altered[0]['exit_code'] = False
        elif mutation=='missing_guard': altered[0].pop('native_batch_guard_pass')
        elif mutation=='bad_binary': altered[0]['binary_provenance']['sha256'] = 'd' * 64
        elif mutation=='bad_generator': altered[0]['generator_sha256'] = 'd' * 64
        elif mutation=='missing_plan_flag': altered[0].pop('correctness_query_plan_required')
        elif mutation=='bad_query_hash': altered[0]['query_sha256'] = 'd' * 64
        elif mutation=='bad_repetition': altered[0]['repetition'] = True
        elif mutation=='bool_nodes': altered[0]['node_count'] = True
        elif mutation=='false_oracle': altered[0]['semantic_oracle_pass'] = False
        try:
            checker.audit(MANIFEST, item['bug'], slice_path, refresh(altered), read,
                          fullsuite_build_path=build_path if item['bug']=='C41' else None)
        except checker.VectorError:
            check(True, 'mutated vector rejected')
        else:
            raise AssertionError('metadata mutation accepted: '+mutation+' '+item['bug'])
    refresh(records)
    for name, value in [('exit_code',False),('cleanup_complete',False),('watchdog_stopped',True),
                        ('stdout_overflow',True),('max_scope_gib',25),('min_host_available_gib',35),
                        ('minimum_host_available_bytes',0)]:
        original = artifacts[guard_path]
        doc = checker.document(original);doc[name]=value;artifacts[guard_path]=encode(doc)
        altered=copy.deepcopy(records)
        for record in altered:record['native_batch_memory_sha256']=checker.sha(artifacts[guard_path])
        try:
            checker.audit(MANIFEST,item['bug'],slice_path,refresh(altered),read,
                          fullsuite_build_path=build_path if item['bug']=='C41' else None)
        except checker.VectorError:check(True,'bound but invalid guard rejected')
        else:raise AssertionError('invalid guard admitted')
        artifacts[guard_path]=original
    refresh(records)
    if item['bug'] in ('C05','C06','C07','C08'):
        changed=copy.deepcopy(records);changed[-1]['repetition']=0
        try:checker.audit(MANIFEST,item['bug'],slice_path,refresh(changed),read)
        except checker.VectorError:check(True,'legacy adapter repetition not silently normalized')
        else:raise AssertionError('legacy adapter promoted')
    if item['bug']=='C02':
        changed=copy.deepcopy(records)
        for record in changed:
            if 'nonvacuous_group_observed' in record:record['nonvacuous_group_observed']=False
        try:checker.audit(MANIFEST,'C02',slice_path,refresh(changed),read)
        except checker.VectorError:check(True,'all-vacuous volatile checks rejected')
        else:raise AssertionError('vacuous volatile vector admitted')
    if item['bug']=='C41':
        try:checker.audit(MANIFEST,'C41',slice_path,refresh(records),read)
        except checker.VectorError:check(True,'runner-only evidence cannot prove full suite')
        else:raise AssertionError('C41 suite omitted')
        original=artifacts[build_path];doc=checker.document(original);doc['target']='ydb/tests/tools/kqprun';artifacts[build_path]=encode(doc)
        try:checker.audit(MANIFEST,'C41',slice_path,refresh(records),read,fullsuite_build_path=build_path)
        except checker.VectorError:check(True,'C41 target requires full suite')
        else:raise AssertionError('C41 runner-only accepted')
        artifacts[build_path]=original

for item in MANIFEST['items']:
    records, artifacts, slice_path, receipt_path, guard_path, build_path, refresh, read = scenario(item)
    def reject(changed, label):
        try:
            checker.audit(MANIFEST,item['bug'],slice_path,refresh(changed),read,
                          fullsuite_build_path=build_path if item['bug']=='C41' else None)
        except checker.VectorError:
            check(True,label)
        else:
            raise AssertionError(label)
    original_receipt = artifacts[receipt_path]
    for mutation in ('incomplete_binary', 'binary_bool_device', 'wrong_worktree', 'stale_receipt'):
        changed = copy.deepcopy(records)
        receipt = checker.document(original_receipt)
        if mutation=='incomplete_binary':
            for name in ('mtime_ns','inode','device','resolved_path'):
                receipt['binary'].pop(name)
                for record in changed:record['binary_provenance'].pop(name)
        elif mutation=='binary_bool_device':
            receipt['binary']['device']=True
            for record in changed:record['binary_provenance']['device']=True
        elif mutation=='wrong_worktree':receipt['worktree']='/wrong/worktree'
        elif mutation=='stale_receipt':receipt['recorded_unix_ns']=MANIFEST['prepared_unix_ns']-1
        artifacts[receipt_path]=encode(receipt)
        for record in changed:record['build_receipt_sha256']=checker.sha(artifacts[receipt_path])
        reject(changed,'incomplete or stale bound receipt rejected')
        artifacts[receipt_path]=original_receipt
    for cell in item['cells']:
        if cell['record_kind']!='generator_snapshot':continue
        selected = next(i for i,r in enumerate(records) if r['case']==cell['case'])
        for field in cell['required_oracle_fields']:
            changed=copy.deepcopy(records)
            changed[selected].pop(field)
            reject(changed,'case-specific successful oracle field required')
    if item['bug']=='C18':
        changed=copy.deepcopy(records)
        fields=('metamorphic_pair','metamorphic_role','metamorphic_projection_sha256',
                'semantic_oracle_scope','legacy_duplicate_empty_set_compatibility')
        groups=[]
        for record in changed:
            if 'metamorphic_pair' in record:
                groups.append({key:record.pop(key) for key in fields})
        unrelated=[record for record in changed if 'two_empty_sets' not in record['case']]
        for target, group in zip(unrelated,groups):target.update(group)
        reject(changed,'C18 roles cannot move to unrelated vector cases')

for raw in (b'{"a":1,"a":2}', b'{"exit_code":0,"exit_code":1}'):
    try:checker.document(raw)
    except checker.VectorError:check(True,'duplicate metadata keys rejected')
    else:raise AssertionError('duplicate metadata accepted')

out = ROOT / 'verification/historical-rerun-vector-metadata-contracts-20261028.json'
report = {'assertions':COUNT,'status':'metadata_only_passed','manifest_sha256':checker.MANIFEST_SHA256,
          'checker_sha256':checker.sha(path.read_bytes()),'native_runs':0,'builds':0,
          'synthetic_metadata_only':True,'live_edits':False,'rendered_workload_persistence':False}
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'assertions':COUNT,'metadata_pass':True,'report_sha256':checker.sha(out.read_bytes())}))
