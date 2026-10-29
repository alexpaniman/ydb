"""Source and mocked-process contracts only; no database/toolchain execution."""
import ast
from contextlib import contextmanager, redirect_stdout
import copy
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
PROJECT=ROOT.parent
CHECKS=[]
SCENARIOS=[]
def sha(raw):return hashlib.sha256(raw).hexdigest()
def check(ok,label):
    if not ok:
        print('metadata_failed_check='+label)
        raise AssertionError(label)
    CHECKS.append(label)
def reject(fn,label):
    try:fn()
    except (ValueError,TypeError,KeyError,OSError):check(True,label)
    else:check(False,label)
def load(raw,path,name):
    m=ModuleType(name);m.__file__=str(path);sys.modules[name]=m
    exec(compile(raw,str(path),'exec'),m.__dict__)
    m._verification_source_sha256=sha(raw)
    return m

def main():
    runner_raw=(HERE/'c06_c50_integration_runner.py').read_bytes()
    gen_raw=(HERE/'c06_c50_bounded_integration_checks.py').read_bytes()
    original_raw=(ROOT/'proposed-correctness-amendments/C06-C50-bounded-integration-20261028/c06_c50_bounded_integration_checks.py').read_bytes()
    def functions(raw):return [ast.dump(n,include_attributes=False) for n in ast.parse(raw).body if isinstance(n,(ast.FunctionDef,ast.AsyncFunctionDef,ast.ClassDef))]
    check(functions(gen_raw)==functions(original_raw),'binding_refresh_preserves_all_generator_function_bodies')
    original=load(original_raw,ROOT/'c06_c50_bounded_integration_checks.py','original_bounded_snapshot')
    refreshed=load(gen_raw,ROOT/'c06_c50_bounded_integration_checks.py','refreshed_bounded_snapshot')
    check(original.RECORDS==refreshed.RECORDS,'refresh_exact_16_vector')
    check(original.PRODUCTION_BINDINGS==refreshed.PRODUCTION_BINDINGS,'refresh_exact_52_production_bindings')
    delta={key for key in original.SOURCE_BINDINGS.keys()|refreshed.SOURCE_BINDINGS.keys() if original.SOURCE_BINDINGS.get(key)!=refreshed.SOURCE_BINDINGS.get(key)}
    check(delta=={'rbo-fixes-20261023/native_verify.py','rbo-fixes-20261023/quality_spill_bounds.py'},'refresh_only_two_dependency_bindings')
    with tempfile.TemporaryDirectory(prefix='c06-c50-runner-metadata-') as td:
        project=Path(td).resolve();root=project/ROOT.name;root.mkdir();(root/'verification').mkdir()
        path=root/'c06_c50_integration_runner.py';path.write_bytes(runner_raw)
        m=load(runner_raw,path,'integration_runner_metadata')
        for relative,expected in m.SOURCE_BINDINGS.items():
            source=HERE/Path(relative).name
            if not source.exists():source=PROJECT/relative
            raw=source.read_bytes();check(sha(raw)==expected,'source_binding_'+sha(relative.encode())[:12])
            dest=project/relative;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(raw)
        snapshots=m._sources();check(len(snapshots)==21,'exact_21_loaded_source_bindings')
        check(sha(m.canonical(list(refreshed.RECORDS)))==m.VECTOR_SHA256,'exact_vector_digest')
        worktree=project/'integration';worktree.mkdir()
        receipt=project/'receipt.json';receipt.write_text('{}')
        udf=worktree/'yql/essentials/udfs/common/math/libmath_udf.so';udf.parent.mkdir(parents=True);udf.write_bytes(b'metadata-only-auxiliary-input')
        driver=m._driver(snapshots,worktree)
        check(driver._verification_source_sha256=='b8553e1ad780a6c87867bd47022233d6047c4d08baf1b75863a43eb34a9cf06b','final_idle53_native_loaded')
        check(driver.g is sys.modules['generate_and_verify'],'driver_uses_loaded_recipe_dependency')
        check(driver.g.BINARY==worktree/'ydb/tests/tools/kqprun/kqprun','driver_bound_to_selected_worktree')
        check(driver.g.MATH==udf,'driver_auxiliary_path_bound')
        check(sys.modules['quality_spill_bounds']._verification_source_sha256=='7cda7ec6c03b18d4a78a152dfc8f53f3fcbbe5487d5df316ac351d0b8e33bc89','final_idle53_quality_adapter_loaded')
        for case in refreshed.CASES:
            marker={'table_service_config':{'EnableQueryServiceSpilling':True}}
            check(sys.modules['quality_spill_bounds'].bound_quality_spill_metadata(None,case,'new',24,marker) is marker,'quality_adapter_leaves_'+case)
        changed=root/'verification_contract.py';saved=changed.read_bytes();changed.write_bytes(saved+b'\n')
        reject(m._sources,'helper_drift_rejected');changed.write_bytes(saved)
        # Only metadata enters these anonymous manifest descriptors.
        manifest_raw=m.canonical({'test':'metadata_only'})
        fd=m._manifest_fd(manifest_raw)
        try:
            check(fcntl.fcntl(fd,fcntl.F_GETFL)&os.O_ACCMODE==os.O_RDONLY,'manifest_read_only')
            check(fcntl.fcntl(fd,fcntl.F_GET_SEALS)==m.SEALS,'manifest_all_four_seals')
            check(m._read_manifest('/proc/self/fd/'+str(fd))=={'test':'metadata_only'},'manifest_sealed_round_trip')
        finally:os.close(fd)
        reject(lambda:m._read_manifest(str(receipt)),'disk_manifest_rejected')
        owned=[];real_create=m.os.memfd_create
        def allocate(*args,**kwargs):
            fd=real_create(*args,**kwargs);owned.append(fd);return fd
        with patch.object(m.os,'memfd_create',side_effect=allocate),patch.object(m.os,'open',side_effect=OSError('metadata-only')):
            reject(lambda:m._manifest_fd(manifest_raw),'manifest_reopen_failure_rejected')
        check(len(owned)==1,'one_manifest_owner_allocated')
        reject(lambda:os.fstat(owned[0]),'failed_manifest_reopen_owner_closed')
        original_read=Path.read_bytes
        def guarded_read(p):
            return b'0::/user.slice/rbo-direct-0123456789ab.scope\n' if str(p)=='/proc/self/cgroup' else original_read(p)
        revision='a'*40
        def spec():return {'version':m.VERSION,'run_id':'b'*32,'nonce':'c'*64,'worktree':str(worktree),'revision':revision,'build_receipt':str(receipt),'receipt_sha256':sha(receipt.read_bytes()),'runner_source_sha256':sha(runner_raw),'source_bindings':m.SOURCE_BINDINGS,'vector_sha256':m.VECTOR_SHA256,'tree':m.EXPECTED_TREE}
        counts={'calls':0,'active':0,'peak':0}
        def make_generator():
            gen=m._load(m.GENERATOR,root/(m.GENERATOR+'.py'),snapshots)
            recipe=gen._load_join_snapshot(gen._source_snapshot())
            provenance={'binary_provenance_verified':True,'binary_provenance_version':'successful_runner_build_v1','build_receipt_path':str(receipt),'build_receipt_sha256':sha(receipt.read_bytes()),'binary_provenance':{'resolved_path':str(driver.g.BINARY.resolve()),'sha256':'d'*64,'size_bytes':1,'mtime_ns':1,'ctime_ns':1,'inode':1,'device':1}}
            scope={'worktree':worktree,'revision':revision,'tree':gen.EXPECTED_TREE,'provenance':provenance,'recipe':recipe,'module_sha256':sha(gen_raw),'driver':driver}
            gen._scope=scope
            gen._require_scope=lambda:scope
            gen.initialize_integration=lambda *args:gen.integration_identity()
            return gen
        def native_record(gen,case,mode,n):
            counts['calls']+=1;counts['active']+=1;counts['peak']=max(counts['peak'],counts['active'])
            try:
                check(mode=='new' and type(n)is int and n==24,'worker_bounded_call_'+str(counts['calls']))
                cell=next(x for x in gen.RECORDS if x['case']==case)
                return dict(cell,generator_module=m.GENERATOR,generator_version=gen.VERSION,generator_sha256=sha(gen_raw),generator_hash_source='loaded_source_snapshot',optimizer_mode_config_verified=True,target_results_checked=True,result_set_boundaries_known=True,expected_rejection_configured=False,table_service_overrides=gen.metadata(case,24)['table_service_config'],exit_code=0,semantic_oracle_pass=True,raw_payload_must_never_be_saved='metadata-only-sentinel')
            finally:counts['active']-=1
        worker_scenarios={}
        for scenario in ('pass','semantic_failure','native_failure','driver_exception','scope_change','cancellation','validation_keyboard_interrupt','validation_interrupted_error'):
            SCENARIOS.append('worker_'+scenario);counts.update(calls=0,active=0,peak=0)
            gen=make_generator();real_identity=gen.integration_identity
            def identity():
                if scenario=='scope_change' and counts['calls']>=3:raise ValueError('metadata-only')
                return real_identity()
            gen.integration_identity=identity
            real_validate=gen.validate_record
            def validate(record,runner):
                if counts['calls']==3:
                    if scenario=='validation_keyboard_interrupt':raise KeyboardInterrupt()
                    if scenario=='validation_interrupted_error':raise InterruptedError('metadata-only')
                return real_validate(record,runner)
            gen.validate_record=validate
            def native(g,c,mode,n):
                row=native_record(g,c,mode,n)
                if counts['calls']==3:
                    if scenario=='semantic_failure':row['semantic_oracle_pass']=False
                    if scenario=='native_failure':row.update(exit_code=7,result_set_boundaries_known=False)
                    if scenario=='driver_exception':raise RuntimeError('metadata-only')
                    if scenario=='cancellation':raise KeyboardInterrupt()
                return row
            output=io.StringIO()
            with patch.object(m,'_expected_revision'),patch.object(m,'_sources',return_value=snapshots),patch.object(m,'_driver',return_value=driver),patch.object(m,'_generator',return_value=gen),patch.object(driver,'run_generated',side_effect=native),patch.object(Path,'read_bytes',guarded_read),redirect_stdout(output):
                try:rc=m._worker(spec())
                except (KeyboardInterrupt,InterruptedError):rc=130
            payload=output.getvalue().encode();records,start,complete,error=m._parse_slice(payload,spec());worker_scenarios[scenario]=payload
            expected_count=2 if scenario=='cancellation' else 3 if scenario=='scope_change' or scenario.startswith('validation_') else 16
            check(len(records)==expected_count,'worker_retains_'+scenario+'_prefix')
            check(counts['peak']==1,'worker_serial_'+scenario)
            check(b'raw_payload_must_never_be_saved' not in payload,'worker_sanitizes_'+scenario)
            check(start['worker_guard_scope_sha256']==sha(guarded_read(Path('/proc/self/cgroup'))),'worker_scope_attested_'+scenario)
            if scenario=='pass':check(rc==0 and complete and error is None and all(r['semantic_oracle_pass'] for r in records),'worker_pass_vector')
            elif scenario in ('semantic_failure','native_failure','driver_exception'):
                check(rc==0 and complete and records[2]['semantic_oracle_pass'] is False,'worker_failed_cell_preserved_'+scenario)
                if scenario!='semantic_failure':check(records[2]['integration_record_validation_pass'] is False,'worker_validation_exception_preserved_'+scenario)
            else:
                check(not complete,'worker_incomplete_'+scenario)
                if scenario.startswith('validation_'):
                    check(records[2]['semantic_oracle_pass'] is False and records[2]['observed_semantic_oracle_pass'] is True,'completed_native_cell_retained_'+scenario)
                    check(records[2]['integration_record_validation_pass'] is False and rc==130,'validation_interruption_reraised_'+scenario)
        baseline=worker_scenarios['pass']
        base_lines=[json.loads(x) for x in baseline.splitlines()]
        for mutation in ('nonce','identity','order','duplicate_start','duplicate_complete','missing_complete','trailing_invalid','old_run'):
            data=copy.deepcopy(base_lines);expected_prefix=16
            if mutation=='nonce':data[3]['nonce']='0'*64;expected_prefix=2
            if mutation=='identity':data[3]['identity_sha256']='0'*64;expected_prefix=2
            if mutation=='order':data[3]['index']=9;expected_prefix=2
            if mutation=='duplicate_start':data.insert(4,copy.deepcopy(data[0]));expected_prefix=3
            if mutation=='duplicate_complete':data.append(copy.deepcopy(data[-1]))
            if mutation=='missing_complete':data.pop()
            if mutation=='trailing_invalid':data.append({'kind':'invalid'})
            if mutation=='old_run':
                for row in data:row['run_id']='0'*32
                expected_prefix=0
            out=b'\n'.join(m.canonical(x) for x in data)+b'\n';rows,start,complete,error=m._parse_slice(out,spec())
            check(len(rows)==expected_prefix,'slice_preserves_valid_prefix_'+mutation)
            check(not complete or error is not None,'slice_cannot_admit_'+mutation)
            SCENARIOS.append('slice_'+mutation)
        # Exact shared serial lease and guard interface are replaced by mocks.
        events=[];guard_fds=[];last_spec={};alter={'kind':'pass'}
        @contextmanager
        def serial_slot():
            events.append('lease_enter')
            try:yield {}
            finally:events.append('lease_exit')
        def guard(command,cwd,fds,**kwargs):
            events.append('guard');check(events[0]=='lease_enter','lease_covers_guard')
            check(command==[sys.executable,'-B',str(path),'--worker','@FD:manifest@'],'exact_guard_command')
            check(cwd==project and set(fds)=={'manifest'},'exact_guard_cwd_and_descriptor')
            check(kwargs==dict(timeout=2220,max_scope_gib=24,min_host_gib=36,retain_stdout=m.LIMIT,compiler_environment=False,metrics_path=kwargs['metrics_path']),'unchanged_guard_limits')
            check(kwargs['metrics_path'].exists() and kwargs['metrics_path'].stat().st_size==0,'fresh_guard_evidence_reserved')
            fd=fds['manifest'];guard_fds.append(fd);s=m._read_manifest('/proc/self/fd/'+str(fd));last_spec.clear();last_spec.update(s)
            if alter['kind']=='guard_exception':raise RuntimeError('metadata-only')
            if alter['kind']=='guard_cancellation':raise KeyboardInterrupt()
            data=copy.deepcopy(base_lines)
            for item in data:
                item.update(run_id=s['run_id'],nonce=s['nonce'],identity_sha256=sha(m.canonical(s)))
                if item['kind']=='record':item['record'].update(integration_run_id=s['run_id'],integration_nonce=s['nonce'])
            kind=alter['kind']
            if kind=='semantic_failure':data[3]['record']['semantic_oracle_pass']=False
            if kind=='validation_failure':data[3]['record']['result_set_boundaries_known']=False;data[3]['record']['semantic_oracle_pass']=False
            if kind=='partial':data=data[:5]
            if kind=='reordered':data[2],data[3]=data[3],data[2]
            if kind=='old_stdout':data=copy.deepcopy(base_lines)
            if kind=='old_scope':data[0]['integration_scope']['runner_sha256']='0'*64
            if kind=='udf_binding':data[0]['auxiliary_udf']['sha256']='0'*64
            stdout=b'\n'.join(m.canonical(item) for item in data)+b'\n'
            metrics={'exit_code':0,'cleanup_complete':True,'watchdog_stopped':False,'max_scope_gib':24,'min_host_available_gib':36,'scope_membership_accounting_available':True,'stdout_sha256':sha(stdout),'stdout_bytes':len(stdout),'stdout_overflow':False}
            if kind=='wrong_stdout_hash':metrics['stdout_sha256']='0'*64
            if kind=='wrong_stdout_size':metrics['stdout_bytes']+=1
            if kind=='overflow':metrics['stdout_overflow']=True
            if kind=='cleanup_failure':metrics['cleanup_complete']=False
            if kind=='watchdog':metrics.update(watchdog_stopped=True,exit_code=75)
            if kind=='wrong_guard_limit':metrics['max_scope_gib']=64
            if kind=='wrong_headroom':metrics['min_host_available_gib']=1
            if kind=='scope_accounting':metrics['scope_membership_accounting_available']=False
            if kind=='bool_exit':metrics['exit_code']=False
            if kind=='receipt_change':receipt.write_text('{"changed":true}')
            if kind=='runner_change':path.write_bytes(runner_raw+b'\n')
            if kind=='source_change':changed.write_bytes(saved+b'\n')
            if kind=='udf_change':udf.write_bytes(b'metadata-only-changed-auxiliary-input')
            return metrics,stdout
        real_load=m._load
        modules={'memory_safe_build':SimpleNamespace(),'anonymous_cpp_guard':SimpleNamespace(run_anonymous=guard),'anonymous_cpp_metadata':SimpleNamespace(),'anonymous_cpp_verify':SimpleNamespace(serial_slot=serial_slot)}
        for kind in ('pass','semantic_failure','validation_failure','partial','reordered','old_stdout','old_scope','udf_binding','wrong_stdout_hash','wrong_stdout_size','overflow','cleanup_failure','watchdog','wrong_guard_limit','wrong_headroom','scope_accounting','bool_exit','receipt_change','runner_change','source_change','udf_change','guard_exception','guard_cancellation','manifest_failure'):
            SCENARIOS.append('outer_'+kind);alter['kind']=kind;events.clear();receipt.write_text('{}');path.write_bytes(runner_raw);changed.write_bytes(saved);udf.write_bytes(b'metadata-only-auxiliary-input')
            # Preserve auxiliary identity across iterations despite metadata test writes.
            aux=m._udf_identity(worktree)
            for item in base_lines:
                if item['kind']=='start':item['auxiliary_udf']=aux
                elif item['kind']=='record':item['record']['auxiliary_udf']=aux
            with patch.object(m,'_load',side_effect=lambda name,*args:modules[name] if name in modules else real_load(name,*args)),patch.object(m,'_expected_revision'),patch.object(m,'_driver',return_value=driver),patch.object(m,'_generator',side_effect=lambda images:make_generator()):
                if kind=='manifest_failure':
                    with patch.object(m,'_manifest_fd',side_effect=OSError('metadata-only')):result=m.run(worktree,revision,receipt)
                else:result=m.run(worktree,revision,receipt)
            check(events[0]=='lease_enter' and events[-1]=='lease_exit','lease_released_'+kind)
            
            check(result['bounded_integration_semantics_pass'] is (kind=='pass'),'outer_admission_'+kind)
            check(result['standalone_fix_admission_changed'] is False and result['final_distribution_proof'] is False,'outer_scope_limits_'+kind)
            records_path=Path(result['records_path']);records=[json.loads(x) for x in records_path.read_bytes().splitlines()]
            check(len(records)==result['record_count'],'saved_prefix_count_'+kind)
            check(sha(records_path.read_bytes())==result['records_sha256'],'saved_prefix_digest_'+kind)
            if kind in ('semantic_failure','validation_failure'):check(len(records)==16 and records[2]['semantic_oracle_pass'] is False,'saved_failed_cell_'+kind)
            if kind=='partial':check(len(records)==4,'saved_partial_vector')
            if kind in ('guard_exception','guard_cancellation','manifest_failure'):check(result['guard_exception_type'] is not None and not result['guard_pass'],'guard_exception_evidence_'+kind)
            if kind!='manifest_failure':reject(lambda:os.fstat(guard_fds[-1]),'outer_manifest_closed_'+kind)
            memory=json.loads(Path(result['memory_path']).read_bytes())
            check(memory['integration_run_id']==result['run_id'],'guard_run_identity_'+kind)
            check(memory['integration_identity_sha256']==result['identity_sha256'],'guard_manifest_identity_'+kind)
            for record in records:check(record['native_batch_memory_sha256']==result['memory_sha256'],'record_guard_digest_'+kind)
        check(len({p.name for p in (root/'verification').glob('*-summary.json')})==24,'fresh_unique_evidence_paths')
        receipt.write_text('{}');path.write_bytes(runner_raw);changed.write_bytes(saved)
        check(len(baseline)<m.LIMIT,'actual_metadata_slice_below_stream_limit')
        # No generated workloads, graph inputs, raw rows or database invocation appear in any mock.
        check('subprocess.run' not in runner_raw.decode(),'runner_has_no_unguarded_native_launch')
        check('run_anonymous(command,PROJECT' in runner_raw.decode(),'runner_uses_reviewed_shared_guard')
    report={'schema':'c06_c50_runner_metadata_contracts_v1','source_sha256':sha(runner_raw),'generator_sha256':sha(gen_raw),'checks_passed':len(CHECKS),'checks':CHECKS,'mocked_scenarios':SCENARIOS,'scenario_count':len(SCENARIOS),'source_only':True,'native_or_build_execution':False,'rendered_workload_persistence':False}
    dest=ROOT/'verification/C06-C50-bounded-integration-runner-metadata-contracts-20261028.json';dest.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    print(json.dumps({'checks_passed':len(CHECKS),'scenarios':len(SCENARIOS),'report_sha256':sha(dest.read_bytes())}))

if __name__=='__main__':main()
