"""Bounded source-only C47 driver checks; all toolchain phases are mocked."""
import copy, hashlib, importlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

R=Path(__file__).resolve().parents[2]
D=Path(__file__).resolve().parent
sys.path.insert(0,str(R))
from anonymous_cpp_metadata import MetadataError
def digest(b): return hashlib.sha256(b).hexdigest()
def load(path,name=None):
    m=ModuleType(name or path.stem);m.__file__=str(path)
    exec(compile(path.read_bytes(),str(path),'exec'),m.__dict__)
    return m

def main():
    checks=[]
    def check(v,name):
        if not v: raise AssertionError(name)
        checks.append(name)
    def rejects(f,name):
        try:f()
        except (ValueError,TypeError,KeyError,OSError,MetadataError):check(True,name)
        else:check(False,name)
    modules_before=dict(sys.modules)
    with tempfile.TemporaryDirectory(prefix='c47-driver-source-only-') as temp:
        T=Path(temp).resolve()
        for name in ('anonymous_cpp_verify.py','correctness_cpp_contract.py','correctness_cpp_profiles.json'):
            (T/name).write_bytes((R/name).read_bytes())
        (T/'c47_lookup_copy_contract_generator.py').write_bytes((R/'c47_lookup_copy_contract_generator.py').read_bytes())
        for name in ('C47-copy-generator-ordered-key-api-20261028.patch',
                     'C47-covering-import-direct-controls-20261028.patch',
                     'C47-covering-import-direct-driver-20261028.patch'):
            result=subprocess.run(['git','apply','-p2','--unsafe-paths','--whitespace=error-all',
                str(R/'proposed-correctness-amendments'/name)],cwd=T,capture_output=True)
            check(result.returncode==0,'patch_applies_'+name+' '+result.stderr.decode())
        for name in ('anonymous_cpp_verify.py','correctness_cpp_contract.py','correctness_cpp_profiles.json',
                     'c47_combined_cpp_generator.py','c47_lookup_copy_contract_generator.py','c47_covering_import_contract_generator.py'):
            check((T/name).read_bytes()==(D/name).read_bytes(),'exact_after_image_'+name)
            if name.endswith('.py'):compile((T/name).read_bytes(),str(T/name),'exec')
        (T/'cases').mkdir()
        for bug in ('C31','C47'):
            (T/'cases'/(bug+'.json')).write_bytes((R/'cases'/(bug+'.json')).read_bytes())
        (T/'c31_identity_cpp_generator.py').write_bytes((R/'c31_identity_cpp_generator.py').read_bytes())
        combined=load(T/'c47_combined_cpp_generator.py')
        components=[combined._load(b)[0] for b in combined.BINDINGS]
        source=combined.render_cpp_suite()
        specs=combined.specs()
        check(len(specs)==51 and len(combined.cases())==51,'exact_combined51')
        check(sum(s['assertion_count'] for s in specs)==796,'exact_assertions796')
        check([s['case'] for s in specs[:19]]==components[0].cases(),'old19_case_names_unchanged')
        check([s['case'] for s in specs[19:]]==components[1].cases(),'new32_case_names_unchanged')
        check(sum(s['assertion_count'] for s in specs[:19])==249,'copy249_assertions')
        check(sum(s['assertion_count'] for s in specs[19:])==547,'covering547_assertions')
        check(source.count('int main()')==1,'single_native_driver')
        check(source.count('namespace C47_copy {')==1 and source.count('namespace C47_covering {')==1,'independent_component_namespaces')
        check(source.rfind('#include ')<source.find('namespace C47_copy {'),'all_headers_at_file_scope')
        for binding,module in zip(combined.BINDINGS,components):
            standalone=module.render_cpp_suite()
            prefix=standalone.split('int main() { unsigned failures = 0;\n')[0]
            exact=''.join(line for line in prefix.splitlines(True) if not line.startswith('#include '))
            check(('namespace C47_'+binding['vector']+' {\n'+exact+'}\n') in source,
                  'exact_component_functions_'+binding['vector'])
            for i,name in enumerate(module.cases()):
                check(source.count('C47_'+binding['vector']+'::Case'+str(i)+'();')==1,'single_dispatch_'+binding['vector']+'_'+name)
        for name in components[0].cases():
            body=components[0]._body(name)
            width=int(name.split('_')[2]) if name.startswith('plain_') and name.count('_')==3 else 3
            loop=body.count('for (size_t index = 0;')
            check(loop in (0,1),'known_copy_loop_'+name)
            expected=body.count('Check(')+(4*(width-1) if loop else 0)
            check(specs[components[0].cases().index(name)]['assertion_count']==expected,'actual_copy_assertions_'+name)
        contract=load(T/'correctness_cpp_contract.py','correctness_cpp_contract')
        sys.modules['correctness_cpp_contract']=contract
        verifier=load(T/'anonymous_cpp_verify.py','anonymous_cpp_verify')
        sys.modules['anonymous_cpp_verify']=verifier
        sys.modules['c47_combined_cpp_generator']=combined
        actual_case=json.loads((T/'cases'/'C47.json').read_bytes())
        rejects(lambda:contract.profile('C47','ebbd0e0aef616c8c3a54d3f92d7c2403e7a1c837'),'profile_rejects_preamendment_revision')
        rejects(lambda:contract.profile('C47','0'*40),'profile_rejects_unbound_revision')
        profile_path=T/'correctness_cpp_profiles.json'
        profiles=json.loads(profile_path.read_bytes())
        # Temporary metadata-only case binding; never saved as actual verification.
        actual_case['commit']=profiles['profiles']['C47']['revision']
        (T/'cases'/'C47.json').write_text(json.dumps(actual_case))
        expected=contract.profile('C47',actual_case['commit'])
        raw,count,ghash=verifier.render_suite('C47',T,actual_case['commit'])
        check(raw==source.encode() and count==51,'render_driver_is_combined_source')
        check(ghash==digest((T/'c47_combined_cpp_generator.py').read_bytes()),'combined_generator_digest')
        check(verifier.GENERATORS['C47']==expected['module'],'driver_generator_registration')
        check(verifier._closure_profile('C47')=='rbo','production_rbo_closure')
        before_c31=json.loads((R/'correctness_cpp_profiles.json').read_bytes())['profiles']['C31']
        check(contract.profile('C31',before_c31['revision'])==before_c31,'C31_profile_unchanged')
        check(contract.render_suite('C31',before_c31['revision'])[1]==33,'C31_render_unchanged')
        records=[dict(case_index=i,checks_executed=s['assertion_count'],passed=True) for i,s in enumerate(specs)]
        encode=lambda rows:b'\n'.join(json.dumps(v).encode() for v in rows)
        check(verifier.parse_verdicts(encode(records),51,0,'C47')==records,'exact51_parser_accepts')
        for index in range(51):
            for delta in (-1,1):
                rows=copy.deepcopy(records);rows[index]['checks_executed']+=delta
                rejects(lambda rows=rows:verifier.parse_verdicts(encode(rows),51,0,'C47'),
                        'exact_assertions_reject_'+str(index)+'_'+str(delta))
        failed=copy.deepcopy(records);failed[2].update(passed=False,checks_executed=1)
        check(verifier.parse_verdicts(encode(failed),51,1,'C47')==failed,'partial_failed_case_is_failure')
        rejects(lambda:verifier.parse_verdicts(encode(failed),51,0,'C47'),'failed_case_cannot_exit_success')
        for rows,code,name in [(records[:-1],0,'missing'),(records+[records[0]],0,'duplicate'),
                               (records,False,'boolean_exit'),(records,1,'wrong_exit')]:
            rejects(lambda rows=rows,code=code:verifier.parse_verdicts(encode(rows),51,code,'C47'),'parser_rejects_'+name)
        for field,value in [('case_index',True),('checks_executed',True),('passed',1),('extra',0)]:
            rows=copy.deepcopy(records);rows[0][field]=value
            rejects(lambda:verifier.parse_verdicts(encode(rows),51,0,'C47'),'parser_rejects_'+field)
        receipt=dict(schema_version=contract.CLOSURE_VERSION,revision=actual_case['commit'],
            build=dict(exit_code=0,revision=actual_case['commit'],profile='rbo',target='ydb/core/kqp/ut/rbo'),
            metadata=dict(profile='rbo',compile_argv=['mock-compile'],compile_cwd=str(T),
                compile_environment={},link_argv=['mock-link','-o','@FD:executable@'],
                link_cwd=str(T),link_environment={}),
            files={'mock-production-object':{'sha256':'a'*64}})
        receipt_path=T/'mock-receipt.json'
        receipt_bytes=json.dumps(receipt).encode()
        # Read the synthetic receipt from memory, never write it.
        read_original=Path.read_bytes
        def read(path):return receipt_bytes if path==receipt_path else read_original(path)
        import production_version_object as version
        phases=[]
        def guarded(argv,worktree,fds,**kw):
            result=dict(exit_code=0,cleanup_complete=True,watchdog_stopped=False,stdout_overflow=False)
            if argv==['mock-compile']:
                phases.append('compile');os.write(fds['object'],b'metadata-only-object')
                check(kw.get('stdout_fd')=='object','anonymous_compile_output_unchanged')
                check(kw.get('timeout')==13,'compile_timeout_unchanged')
                result['environment_sha256']=verifier._json_digest({})
                return result,b''
            if argv[0]=='mock-link':
                phases.append('link');os.write(fds['executable'],b'metadata-only-executable')
                check(kw.get('stdout_fd')=='executable' and kw.get('max_scope_gib')==36,'anonymous_link_guard_unchanged')
                check('-Wl,--strip-debug' in argv and argv[argv.index('-o')+1]=='-','reviewed_link_transport_unchanged')
                check(kw.get('timeout')==17,'link_timeout_unchanged')
                result['environment_sha256']=verifier._json_digest({})
                return result,b''
            check(argv==['@FD:executable@'],'native_memfd_only_unchanged')
            phases.append('native')
            check(kw.get('max_scope_gib')==12 and kw.get('timeout')==19,'native_guard_unchanged')
            return result,encode(records)
        args=SimpleNamespace(bug='C47',phase='fixed',worktree=T,receipt=receipt_path,
                             compile_timeout=13,link_timeout=17,native_timeout=19)
        with patch.object(Path,'read_bytes',read), \
             patch.object(verifier,'_expected_revision',return_value=actual_case['commit']), \
             patch.object(verifier,'verify_receipt',return_value=True), \
             patch.object(verifier,'run_anonymous',guarded), \
             patch.object(version,'prepare_object',return_value=(list(receipt['metadata']['link_argv']),{})), \
             patch.object(version,'verify_object',return_value=True), \
             patch.object(version,'validate_evidence',return_value=True):
            evidence,code=verifier.execute(args)
            check(code==0 and evidence['status']=='direct_local_contracts_passed','mocked_complete_driver_wiring')
            check(phases==['compile','link','native'],'single_serial_mocked_child_per_phase')
            check(verifier.validate_fixed_admission(evidence,'C47',actual_case['commit']),'mocked_complete_admission')
            check(evidence['query_semantics_verified'] is False and evidence['quality_improvement_verified'] is False,
                  'direct_evidence_does_not_claim_SQL_or_quality')
            for field,value in [('direct_case_vector_sha256','b'*64),('direct_contract_vectors',[]),
                                ('generator_module','c47_lookup_copy_contract_generator'),
                                ('case_count',19),('generated_source_sha256','b'*64),
                                ('direct_contract_profile_sha256','b'*64),
                                ('revision',actual_case['base']),('generated_files_persisted',True)]:
                changed=copy.deepcopy(evidence);changed[field]=value
                check(not verifier.validate_fixed_admission(changed,'C47',actual_case['commit']),'admission_rejects_'+field)
            for phase in ('compile','link','native'):
                for field,value in [('exit_code',1),('exit_code',False),('cleanup_complete',False),
                                    ('watchdog_stopped',True),('stdout_overflow',True)]:
                    changed=copy.deepcopy(evidence);changed[phase][field]=value
                    check(not verifier.validate_fixed_admission(changed,'C47',actual_case['commit']),'guard_rejects_'+phase+'_'+field+'_'+str(value))
            for index in (0,18,19,50):
                changed=copy.deepcopy(evidence);changed['cases'][index]['checks_executed']-=1
                check(not verifier.validate_fixed_admission(changed,'C47',actual_case['commit']),'admission_count_'+str(index))
        for binding in combined.BINDINGS:
            path=T/(binding['module']+'.py');old=path.read_bytes();path.write_bytes(old+b'\n')
            rejects(lambda:verifier.render_suite('C47',T,actual_case['commit']),'component_source_mutation_'+binding['vector'])
            path.write_bytes(old)
        for field in ('case_assertions','vectors'):
            changed=copy.deepcopy(profiles)
            if field=='case_assertions':changed['profiles']['C47'][field][0]['assertion_count']+=1
            else:changed['profiles']['C47'][field][0]['generator_sha256']='a'*64
            profile_path.write_text(json.dumps(changed))
            rejects(lambda:verifier.render_suite('C47',T,actual_case['commit']),'profile_vector_mutation_'+field)
            profile_path.write_text(json.dumps(profiles))
        # The current runner is fixed-only before any toolchain action.
        with patch.object(verifier.subprocess,'check_output') as git:
            rejects(lambda:verifier._expected_revision('C47','baseline',T),'baseline_rejected')
            check(git.call_count==0,'baseline_rejection_before_git')
    for key in ('correctness_cpp_contract','anonymous_cpp_verify','c47_combined_cpp_generator'):
        if key in modules_before:sys.modules[key]=modules_before[key]
        else:sys.modules.pop(key,None)
    result=dict(status='source_and_mocked_metadata_checks_passed_native_pending',
                check_count=len(checks),checks=checks,native_processes_started=0,toolchains_started=0,
                synthetic_evidence_persisted=False,generated_sources_persisted=False,
                live_helpers_changed=False)
    target=R/'verification/C47-covering-import-direct-driver-metadata-checks-20261028.json'
    target.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='checks'}))
if __name__=='__main__':main()
