"""Metadata-only integration wrapper checks; subprocesses are read-only Git."""
import ast
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PROJECT = ROOT.parent
sys.path.insert(0, str(ROOT))
import native_verify


def sha(x):
    return hashlib.sha256(x).hexdigest()


def load(source, filename):
    module = ModuleType('c06_c50_bounded_integration_checks')
    module.__file__ = str(filename)
    exec(compile(source, str(filename), 'exec'), module.__dict__)
    module._verification_source_sha256 = sha(source)
    return module


def main():
    checks = []
    def check(ok, label):
        if not ok: raise AssertionError(label)
        checks.append(label)
    def reject(fn, label):
        try: fn()
        except (ValueError, TypeError, KeyError, OSError): check(True, label)
        else: check(False, label)
    path = HERE/'c06_c50_bounded_integration_checks.py'
    raw = path.read_bytes()
    original = load(raw, ROOT/path.name)
    original_preparation = json.loads((ROOT/'verification/C06-C50-integration-source-preparation-20261028.json').read_bytes())
    production = json.loads((ROOT/'verification/C06-C50-bounded-spill-production-source-comparison-20261028.json').read_bytes())
    source_images = {}
    for name, digest in original.PRODUCTION_BINDINGS.items():
        revision = production['changed_files'].get(name, {}).get('revision', production['base'])
        image = subprocess.check_output(['git', 'show', revision+':'+name], cwd=PROJECT)
        check(sha(image) == digest, 'production_image_' + sha(name.encode())[:12])
        source_images[name] = image
    with tempfile.TemporaryDirectory(prefix='c06-c50-wrapper-metadata-') as td:
        project = Path(td).resolve()
        root = project/ROOT.name
        root.mkdir()
        for name, digest in original.SOURCE_BINDINGS.items():
            source = PROJECT/name
            destination = project/name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())
            check(sha(destination.read_bytes()) == digest, 'snapshot_' + sha(name.encode())[:12])
        candidate = root/path.name
        candidate.write_bytes(raw)
        m = load(raw, candidate)
        reject(lambda:m.recipes(24), 'uninitialized_workloads_rejected')
        reject(lambda:m.metadata(m.CASES[0],24), 'uninitialized_metadata_rejected')
        reject(lambda:m.schema_and_seeds(24,m.CASES[0]), 'uninitialized_schema_rejected')
        reject(lambda:m.check_rows(m.CASES[0],[],24), 'uninitialized_oracle_rejected')
        worktree = project/'integration-tree'
        worktree.mkdir()
        runner = worktree/'ydb/tests/tools/kqprun/kqprun'
        runner.parent.mkdir(parents=True)
        receipt = project/'receipt.json'
        receipt.write_text('{}')
        revision = 'a'*40
        provenance = {'binary_provenance_verified':True,
            'binary_provenance_version':'successful_runner_build_v1',
            'build_receipt_path':str(receipt), 'build_receipt_sha256':sha(receipt.read_bytes()),
            'binary_provenance':{'resolved_path':str(runner),'sha256':'b'*64,'size_bytes':1,
                                 'mtime_ns':1,'ctime_ns':1,'inode':1,'device':1}}
        calls=[]
        class Provenance:
            @staticmethod
            def verify_build_receipt(w, r, p):
                calls.append((str(w),r,str(p)))
                if w != worktree or r != revision or Path(p) != receipt: raise ValueError('mock provenance binding')
                return copy.deepcopy(provenance)
            @staticmethod
            def unchanged_since_verification(w,r,p):
                return w==worktree and r==revision and p==provenance
        def git(argv, *, cwd, text=False):
            check(cwd==worktree,'init_git_worktree')
            if argv==['git','rev-parse','HEAD']: return revision+'\n'
            if argv==['git','rev-parse','HEAD^{tree}']: return m.EXPECTED_TREE+'\n'
            if argv[:2]==['git','show']:
                prefix,name=argv[2].split(':',1)
                if prefix!=revision: raise ValueError('mock source revision')
                return source_images[name]
            raise ValueError('unreviewed subprocess command')
        driver = ModuleType('native_verify')
        driver.__file__ = str(root/'native_verify.py')
        driver._verification_source_sha256 = m.SOURCE_BINDINGS['rbo-fixes-20261023/native_verify.py']
        driver.g = SimpleNamespace(BINARY=runner)
        for variant in ('wrong_tree','changed_production_image','wrong_driver_path','wrong_driver_snapshot'):
            other=load(raw,candidate)
            other_driver=ModuleType('native_verify');other_driver.__dict__.update(driver.__dict__)
            other_driver.g=SimpleNamespace(BINARY=runner)
            if variant=='wrong_driver_path':other_driver.g.BINARY=worktree/'other'
            if variant=='wrong_driver_snapshot':other_driver._verification_source_sha256='0'*64
            def bad_git(argv,**kwargs):
                if variant=='wrong_tree' and argv==['git','rev-parse','HEAD^{tree}']:return '0'*40+'\n'
                if variant=='changed_production_image' and argv[:2]==['git','show']:return b'changed'
                return git(argv,**kwargs)
            with patch.object(other,'_load_provenance',return_value=Provenance), patch.object(other.subprocess,'check_output',side_effect=bad_git):
                reject(lambda:other.initialize_integration(worktree,receipt,other_driver),'init_reject_'+variant)
            check(other._scope is None,'failed_init_leaves_no_scope_'+variant)
        calls.clear()
        with patch.object(m,'_load_provenance',return_value=Provenance), patch.object(m.subprocess,'check_output',side_effect=git):
            identity=m.initialize_integration(worktree,receipt,driver)
        check(len(calls)==1,'one_fresh_provenance_verification')
        check(identity['tree']==production['exact_expected_integration_tree'],'exact_tree_bound')
        check(identity['runner_sha256']=='b'*64 and identity['build_receipt_sha256']==sha(receipt.read_bytes()),'exact_executable_receipt_bound')
        reject(lambda:m.initialize_integration(worktree,receipt,driver),'repeat_initialization_rejected')
        original_generator=m._scope['recipe']
        recipes=m.recipes(24)
        check(list(recipes)==list(m.CASES) and len(recipes)==8,'exact8_case_order')
        check(len(m.required_record_vector())==16,'exact16_records')
        records=[]
        for index,expected in enumerate(m.RECORDS):
            before=original_preparation['expected_records'][index]
            case=expected['case']
            meta=m.metadata(case,24,'new')
            old_meta=original_generator.requirements(case,'new')
            expected_meta=copy.deepcopy(old_meta)
            expected_meta['table_service_config'].update(EnableSpillingNodes='None',SpillingServiceConfig=m.LOCAL_SERVICE)
            expected_meta['generator_version']=m.VERSION
            check(meta==expected_meta,'only_bounded_metadata_delta_'+str(index))
            check(meta['node_count']==4 and meta['repetitions']==2,'four_nodes_two_repeats_'+str(index))
            cfg=meta['table_service_config']
            for name in ('EnableQueryServiceSpilling','EnableSpillingInHashJoinShuffleConnections','UseBlockHashJoin'):
                check(cfg[name] is True,'preserved_flag_'+str(index)+'_'+name)
            schema,seeds=m.schema_and_seeds(24,case)
            pragmas=dict(meta['pragmas']);pragmas['YqlSelect']='force'
            query=''.join('PRAGMA '+key+'='+native_verify.g.quote(value)+'; ' for key,value in pragmas.items())+recipes[case]+';'
            settings=native_verify.config_with((project/'rbo-review-20261021/new.conf').read_text(),cfg)
            hashes={'query_sha256':sha(query.encode()),'schema_sha256':sha(schema.encode()),
                    'fixture_sha256':[sha((x+';').encode()) for x in seeds], 'settings_sha256':sha(settings.encode())}
            for name in ('query_sha256','schema_sha256','fixture_sha256'):
                check(hashes[name]==before[name]==expected[name],'original_workload_slot_'+str(index)+'_'+name)
            check(hashes['settings_sha256']==expected['settings_sha256']!=before['settings_sha256'],'changed_settings_slot_'+str(index))
            rows=original_generator.expected_rows(case,24)
            check(m.check_rows(case,rows,24)==original_generator.check_rows(case,rows,24)==True,'unchanged_positive_bag_oracle_'+str(index))
            altered=rows+[{'unexpected':0}]
            check(m.check_rows(case,altered,24)==original_generator.check_rows(case,altered,24)==False,'unchanged_negative_bag_oracle_'+str(index))
            record=dict(expected,revision=revision,phase='integration',generator_sha256=m._verification_source_sha256,
                        generator_module='c06_c50_bounded_integration_checks',generator_version=m.VERSION,
                        generator_hash_source='loaded_source_snapshot',
                        integration_scope=m.integration_identity(),optimizer_mode_config_verified=True,
                        target_results_checked=True,result_set_boundaries_known=True,expected_rejection_configured=False,
                        table_service_overrides=cfg,exit_code=0,semantic_oracle_pass=True,**provenance)
            check(m.validate_record(record,runner) is True,'valid_metadata_record_'+str(index))
            for field,value in [('settings_sha256',before['settings_sha256']),('phase','fixed'),('node_count',1),
                                ('revision','c'*40),('generator_sha256','d'*64),('repetition',9),('query_sha256','0'*64),
                                ('repetition',False),('exit_code',False),('generator_module','join_key_checks')]:
                wrong=copy.deepcopy(record);wrong[field]=value
                reject(lambda:m.validate_record(wrong,runner),'record_binding_reject_'+str(index)+'_'+field)
            wrong=copy.deepcopy(record);wrong['semantic_oracle_pass']=False
            check(m.validate_record(wrong,runner) is False,'semantic_failure_preserved_'+str(index))
            records.append(record)
        for n in (True,0,8,23,25,48):
            reject(lambda:m.recipes(n),'unreviewed_size_'+str(n))
        reject(lambda:m.metadata(m.CASES[0],24,'legacy'),'legacy_rejected')
        reject(lambda:m.metadata('c50_left_semi_uncorrelated',24),'extra_case_rejected')
        reject(lambda:m.validate_record(records[0],worktree/'other'),'wrong_driver_path_rejected')
        guard=project/'guard.json'
        memory={'exit_code':0,'max_scope_gib':24,'min_host_available_gib':36,'cleanup_complete':True,
                'watchdog_stopped':False,'stdout_overflow':False,'scope_membership_accounting_available':True}
        guard.write_text(json.dumps(memory))
        for record in records:
            record.update(native_batch_guard_pass=True,native_batch_memory_evidence=str(guard),native_batch_memory_sha256=sha(guard.read_bytes()))
        result=m.validate_complete(records)
        check(result['bounded_integration_semantics_pass'] is True and result['semantic_pass_count']==16,'mock_complete_vector_transport_only')
        check(result['configuration_changed'] is True and result['final_distribution_proof'] is False and result['standalone_fix_admission_changed'] is False,'scope_flags_preserved')
        for bad in (records[:-1],records[::-1],records[:-1]+[records[0]]):
            reject(lambda:m.validate_complete(bad),'incomplete_or_wrong_vector_'+str(len(checks)))
        for field,value in [('max_scope_gib',25),('min_host_available_gib',35),('watchdog_stopped',True),('cleanup_complete',False),('exit_code',1)]:
            changed=dict(memory);changed[field]=value;guard.write_text(json.dumps(changed))
            for record in records:record['native_batch_memory_sha256']=sha(guard.read_bytes())
            reject(lambda:m.validate_complete(records),'bad_guard_'+field)
        guard.write_text(json.dumps(memory))
        for record in records:record['native_batch_memory_sha256']=sha(guard.read_bytes())
        records[0]['semantic_oracle_pass']=False
        check(m.validate_complete(records)['bounded_integration_semantics_pass'] is False,'complete_semantic_failure_preserved')
        driver.g.BINARY=worktree/'other'
        reject(lambda:m.recipes(24),'driver_changed_before_workload_rejected')
        driver.g.BINARY=runner
        receipt.write_text('{"changed":true}')
        reject(lambda:m.recipes(24),'receipt_changed_before_workload_rejected')
        receipt.write_text('{}')
        helper=root/'join_key_checks.py';before=helper.read_bytes();helper.write_bytes(before+b'\n')
        reject(lambda:m.recipes(24),'source_changed_before_workload_rejected')
        helper.write_bytes(before)
        loaded=m._scope['recipe']
        check(loaded._verification_source_sha256==original.SOURCE_BINDINGS['rbo-fixes-20261023/join_key_checks.py'],'loaded_join_bytes_bound')
        check(loaded.w._verification_source_sha256==original.SOURCE_BINDINGS['rbo-fixes-20261023/window_checks.py'],'loaded_window_bytes_bound')
        check(loaded.g._verification_source_sha256==original.SOURCE_BINDINGS['rbo-review-20261021/generate_and_verify.py'],'loaded_builder_bytes_bound')
        check(m.integration_identity()['source_snapshot_sha256']==original.SOURCE_BINDINGS,'all_snapshot_bindings_attested')
    report={'schema':'c06_c50_bounded_integration_metadata_v1','checks_passed':len(checks),'checks':checks,
            'mock_records_are_not_native_evidence':True,'native_build_runs':0,'only_subprocesses':'read-only Git source reads',
            'rendered_queries_fixtures_rows_or_cpp_persisted':False,'changed_live_sources':False,
            'unchanged_query_schema_fixture_slots':48,'deliberately_changed_settings_slots':16}
    p=ROOT/'verification/C06-C50-bounded-integration-metadata-contracts-20261028.json';p.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'checks_passed':len(checks),'report':str(p),'sha256':sha(p.read_bytes())}))

if __name__=='__main__': main()
