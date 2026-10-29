"""Synthetic metadata rejection checks only; no new workload or native process."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
encode = lambda value: json.dumps(value, sort_keys=True).encode()
sha = lambda raw: hashlib.sha256(raw).hexdigest()


def main():
    checks = []
    def check(value, name):
        if not value:
            raise AssertionError(name)
        checks.append(name)
    path = HERE/'c41_amended_exact_vector_check_20261028.py'
    spec = importlib.util.spec_from_file_location('c41_amended_check', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    manifest = json.loads(module.MANIFEST_PATH.read_bytes())
    original = json.loads((ROOT/'verification/historical-rerun-exact-vector-20261028.json').read_bytes())
    original_item = copy.deepcopy(next(i for i in original['items'] if i['bug']=='C41'))
    original_item['revision'] = module.REVISION
    check(manifest['items'] == [original_item], 'original12_vector_exact_except_revision')
    check(sha(module.MANIFEST_PATH.read_bytes()) == module.MANIFEST_SHA256, 'frozen_manifest')
    check(sha(module.LEGACY_PATH.read_bytes()) == module.LEGACY_SHA256, 'unchanged_legacy_checker')
    # Reuse only the existing metadata scenario factory, before its test loop.
    factory_path = HERE/'historical_rerun_vector_contracts_20261028.py'
    prefix = factory_path.read_text().split("for item in MANIFEST['items']:",1)[0]
    scope = {'__file__':str(factory_path),'__name__':'source_only_scenario_factory'}
    exec(compile(prefix,str(factory_path),'exec'),scope)
    scope['MANIFEST'] = manifest
    item = manifest['items'][0]
    records, artifacts, slice_path, receipt_path, guard_path, build_path, refresh, read = scope['scenario'](item)
    receipt = json.loads(artifacts[receipt_path])
    receipt['binary']['resolved_path'] = str((Path(manifest['expected_worktree'])/'ydb/tests/tools/kqprun/kqprun').resolve())
    artifacts[receipt_path] = encode(receipt)
    guard = json.loads(artifacts[guard_path]);guard.update(peak_scope_usage_bytes=1,peak_group_rss_bytes=1)
    artifacts[guard_path] = encode(guard)
    build = json.loads(artifacts[build_path]);build['memory'].update(
        peak_scope_usage_bytes=1,peak_group_rss_bytes=1,minimum_host_available_bytes=40*1024**3)
    artifacts[build_path] = encode(build)
    for record in records:
        record['binary_provenance'] = dict(receipt['binary'])
        record['build_receipt_sha256'] = sha(artifacts[receipt_path])
        record['native_batch_memory_sha256'] = sha(artifacts[guard_path])
    refresh(records)
    accepted = module.audit(slice_path,build_path,read)
    check(accepted['exact_vector_passed'] is True and accepted['checked_records']==12
          and accepted['C41_fullsuite_build_bound'] is True
          and accepted['automatic_full_finding_promotion'] is False,
          'complete_mock12_plus_fullsuite_remains_scoped')
    original_artifacts = dict(artifacts)
    def reject(values, label, reader=None):
        refresh(values)
        try:
            module.audit(slice_path,build_path,reader or read)
        except Exception:
            check(True,label)
        else:
            check(False,label)
        artifacts.clear();artifacts.update(original_artifacts)
    for i,cell in enumerate(item['cells']):
        for key in (*cell['required_oracle_fields'],*cell['required_record_fields'],
                    'semantic_oracle_pass','binary_provenance_required','native_batch_guard_pass',
                    'generator_sha256','query_sha256','fixture_sha256','settings_sha256',
                    'validation_sha256','result_sha256','result_set_count'):
            changed=copy.deepcopy(records);changed[i].pop(key,None)
            reject(changed,'case_specific_missing_field_rejected')
        for key,value in [('exit_code',False),('revision',original['items'][-1]['revision']),
                          ('rows',True),('repetition',True),('node_count',True),('mode','legacy')]:
            changed=copy.deepcopy(records);changed[i][key]=value
            reject(changed,'case_identity_type_or_revision_rejected')
    for changed in (records[:-1],records+[records[0]],records[:-1]+[records[0]]):
        reject(copy.deepcopy(changed),'incomplete_extra_duplicate_vector_rejected')
    for key in ('sha256','size_bytes','ctime_ns','mtime_ns','inode','device','resolved_path'):
        changed=copy.deepcopy(records)
        for record in changed:record['binary_provenance'].pop(key)
        reject(changed,'missing_canonical_binary_field_rejected')
    for key,value in [('exit_code',False),('cleanup_complete',False),('watchdog_stopped',True),
                      ('stdout_overflow',True),('max_scope_gib',25),('min_host_available_gib',35),
                      ('minimum_host_available_bytes',0),('peak_scope_usage_bytes',25*1024**3),
                      ('peak_group_rss_bytes',True)]:
        doc=json.loads(original_artifacts[guard_path]);doc[key]=value;artifacts[guard_path]=encode(doc)
        changed=copy.deepcopy(records)
        for record in changed:record['native_batch_memory_sha256']=sha(artifacts[guard_path])
        reject(changed,'invalid_guard_even_with_updated_digest_rejected')
    for key,value in [('target','ydb/tests/tools/kqprun'),('target','ydb/tests/tools/kqprun ydb/core/kqp/ut/rbo -tA -F subset'),
                      ('revision','b71ac02fc35f8a058faf2eb996ca8cf0d0266a18'),('exit_code',False),
                      ('diagnostic_sha256',''),('elapsed_seconds',True)]:
        doc=json.loads(original_artifacts[build_path]);doc[key]=value;artifacts[build_path]=encode(doc)
        receipt_doc=json.loads(original_artifacts[receipt_path])
        if key in receipt_doc['build']:receipt_doc['build'][key]=value
        artifacts[receipt_path]=encode(receipt_doc)
        changed=copy.deepcopy(records)
        for record in changed:record['build_receipt_sha256']=sha(artifacts[receipt_path])
        reject(changed,'invalid_fullsuite_even_with_receipt_rebinding_rejected')
    for key,value in [('explicit_build_workers',64),('watchdog_stopped',True),('cleanup_complete',False),
                      ('watchdog_min_host_available_gib',35),('watchdog_max_scope_usage_gib',53),
                      ('minimum_host_available_bytes',0),('peak_scope_usage_bytes',53*1024**3),
                      ('peak_group_rss_bytes',True),('guard_exception_type','InterruptedError'),('stop_reason','guard_cancelled')]:
        doc=json.loads(original_artifacts[build_path]);doc['memory'][key]=value;artifacts[build_path]=encode(doc)
        reject(copy.deepcopy(records),'invalid_fullsuite_guard_rejected')
    for binding in manifest['source_bindings']:
        target=Path(binding['path'])
        reject(copy.deepcopy(records),'changed_installed_source_binding_rejected',
               lambda p,t=target:b'metadata-only-different-source' if p==t else read(p))
    worktree=Path(manifest['expected_worktree'])
    for relative in manifest['suite_source_bindings']:
        target=worktree/relative
        reject(copy.deepcopy(records),'changed_existing_suite_source_rejected',
               lambda p,t=target:b'metadata-only-different-source' if p==t else read(p))
    target=worktree/manifest['required_production_boundary']['path']
    reject(copy.deepcopy(records),'old_production_boundary_rejected',
           lambda p:b'metadata-only-old-boundary' if p==target else read(p))
    original_bytes = original_artifacts[receipt_path]
    doc=json.loads(original_bytes);doc['recorded_unix_ns']=manifest['prepared_unix_ns']-1
    artifacts[receipt_path]=encode(doc);changed=copy.deepcopy(records)
    for record in changed:record['build_receipt_sha256']=sha(artifacts[receipt_path])
    reject(changed,'historical_receipt_reuse_rejected')
    report={'schema':'C41_amended_metadata_contracts_v1','checks_passed':len(checks),'checks':checks,
            'actual_native_build_or_git_runs':0,'new_workloads_created':False,
            'synthetic_evidence_persisted':False,'runtime_admission_created':False}
    print(json.dumps(report,sort_keys=True))


if __name__=='__main__':
    main()
