"""Mock C31/C47 anonymous execution wiring; never run a compiler or binary."""
import copy
import hashlib
import json
import os
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

import correctness_cpp_contract as contract
import prepare_correctness_cpp_verifier_patch as prepared

ROOT = Path(__file__).resolve().parent


def main():
    text=prepared.render_files()['anonymous_cpp_verify.py'][1]
    verifier=ModuleType('prospective_correctness_cpp_verify')
    verifier.__file__=str(ROOT/'anonymous_cpp_verify.py')
    exec(compile(text,verifier.__file__,'exec'),verifier.__dict__)
    assertions=0
    phase_calls=[]
    def check(value):
        nonlocal assertions
        assert value
        assertions+=1
    original_read=Path.read_bytes
    for bug in ('C31','C47'):
        case=json.loads((ROOT/'cases'/(bug+'.json')).read_bytes())
        revision=case['commit'];profile=contract.profile(bug,revision)
        check(verifier.GENERATORS[bug]==profile['module'])
        check(verifier._closure_profile(bug)=='rbo')
        with patch.object(verifier.subprocess,'check_output',return_value=case['base']+'\n') as git:
            check(verifier._expected_revision(bug,'fixed',ROOT)==revision)
            check(git.call_count==1)
        with patch.object(verifier.subprocess,'check_output') as git:
            try:verifier._expected_revision(bug,'baseline',ROOT)
            except verifier.MetadataError:check(True)
            else:check(False)
            check(git.call_count==0)
        try:verifier.pair_direct(bug,{}, {},case)
        except verifier.MetadataError:check(True)
        else:check(False)
        source,count,digest=verifier.render_suite(bug,ROOT,revision)
        check(count==profile['case_count'] and digest==profile['generator_sha256'])
        check(hashlib.sha256(source).hexdigest()==profile['generated_source_sha256'])
        del source
        records=[dict(case_index=index,checks_executed=1,passed=True) for index in range(count)]
        receipt=dict(schema_version=contract.CLOSURE_VERSION,revision=revision,
                     build=dict(exit_code=0,revision=revision,profile='rbo',target='ydb/core/kqp/ut/rbo'),
                     metadata=dict(profile='rbo',compile_argv=['mock-compile'],compile_cwd=str(ROOT),
                                   link_argv=['mock-link'],link_cwd=str(ROOT)),
                     files={'mock-production-object':{'sha256':'a'*64}})
        raw_receipt=json.dumps(receipt).encode()
        mock_receipt_path=Path('/anonymous-mocked-correctness-closure')
        def read(path):
            return raw_receipt if path==mock_receipt_path else original_read(path)
        def guarded(command,worktree,fds,**kwargs):
            if command==['mock-compile']:
                stage='compile';os.write(fds['object'],b'mocked-object');out=b''
                check(kwargs.get('stdout_fd')=='object')
            elif command[0]=='mock-link':
                stage='link';os.write(fds['executable'],b'mocked-executable');out=b''
                check(kwargs.get('max_scope_gib')==36)
            else:
                stage='native';out=b'\n'.join(json.dumps(value).encode() for value in records)
                check(command==['@FD:executable@'] and kwargs.get('max_scope_gib')==12)
            phase_calls.append((bug,stage))
            return dict(exit_code=0,cleanup_complete=True,watchdog_stopped=False,stdout_overflow=False),out
        args=SimpleNamespace(bug=bug,phase='fixed',worktree=ROOT,receipt=mock_receipt_path,
                             compile_timeout=900,link_timeout=900,native_timeout=120)
        with patch.object(Path,'read_bytes',read), \
             patch.object(verifier,'_expected_revision',return_value=revision), \
             patch.object(verifier,'verify_receipt',return_value=True) as closure, \
             patch.object(verifier,'run_anonymous',guarded):
            evidence,code=verifier.execute(args)
            check(code==0 and evidence['status']=='direct_local_contracts_passed')
            check(closure.call_count==2)
            check(evidence['case_count']==count and len(evidence['cases'])==count)
            check(evidence['query_semantics_verified'] is False)
            check(verifier.validate_fixed_admission(evidence,bug,revision))
            for field,value in (('phase','baseline'),('revision',case['base']),
                                ('generator_sha256','b'*64),('generated_source_sha256','b'*64),
                                ('direct_contract_profile_sha256','b'*64),('generator_module','unreviewed'),
                                ('case_count',True),('generated_source_bytes',0),
                                ('query_semantics_verified',True),('generated_files_persisted',True),
                                ('status','prepared_unverified'),('closure_receipt_sha256','b'*64)):
                changed=copy.deepcopy(evidence);changed[field]=value
                check(not verifier.validate_fixed_admission(changed,bug,revision))
            for phase in ('compile','link','native'):
                for field,value in (('exit_code',1),('exit_code',False),('cleanup_complete',False),
                                    ('watchdog_stopped',True),('stdout_overflow',True)):
                    changed=copy.deepcopy(evidence);changed[phase][field]=value
                    check(not verifier.validate_fixed_admission(changed,bug,revision))
                changed=copy.deepcopy(evidence);del changed[phase]
                check(not verifier.validate_fixed_admission(changed,bug,revision))
            for field,value in (('case_index',True),('checks_executed',0),('passed',False),('extra',0)):
                changed=copy.deepcopy(evidence);changed['cases'][0][field]=value
                check(not verifier.validate_fixed_admission(changed,bug,revision))
            changed=copy.deepcopy(evidence);changed['cases'].pop()
            check(not verifier.validate_fixed_admission(changed,bug,revision))
            for field,value in (('exit_code',False),('exit_code',1),('revision',case['base']),
                                ('profile','ast'),('target','ydb/tests/tools/kqprun')):
                altered=copy.deepcopy(receipt);altered['build'][field]=value
                raw_receipt=json.dumps(altered).encode()
                changed=copy.deepcopy(evidence);changed['closure_receipt_sha256']=hashlib.sha256(raw_receipt).hexdigest()
                check(not verifier.validate_fixed_admission(changed,bug,revision))
        raw=b'\n'.join(json.dumps(record).encode() for record in records)
        check(verifier.parse_verdicts(raw,count,0,bug)==records)
        for stdout,exit_code in ((raw,False),(raw,1),(raw+b'\n'+raw.splitlines()[0],0),
                                 (b'{"case_index":0,"case_index":0,"checks_executed":1,"passed":true}',0)):
            try:verifier.parse_verdicts(stdout,count,exit_code,bug)
            except verifier.MetadataError:check(True)
            else:check(False)
        # Changing reviewed generator bytes must fail before rendering; this
        # mock neither changes the file nor publishes a generated translation unit.
        generator_path=ROOT/(profile['module']+'.py')
        def altered_generator(path):
            content=original_read(path)
            return content+b'\n' if path==generator_path else content
        with patch.object(Path,'read_bytes',altered_generator):
            try:verifier.render_suite(bug,ROOT,revision)
            except verifier.MetadataError:check(True)
            else:check(False)
        try:verifier.render_suite(bug,ROOT,case['base'])
        except verifier.MetadataError:check(True)
        else:check(False)
    check(phase_calls==[(bug,stage) for bug in ('C31','C47') for stage in ('compile','link','native')])
    report=dict(status='mocked_integration_passed_actual_toolchain_pending',assertions=assertions,
                mocked_process_phases=len(phase_calls),actual_compiler_runs=0,actual_native_runs=0,
                active_helpers_edited=False,production_sources_edited=False,
                generated_sources_objects_executables_persisted=False,synthetic_evidence_saved=False,
                patch_sha256=hashlib.sha256(prepared.render_patch().encode()).hexdigest(),
                helper_sha256=hashlib.sha256(Path(contract.__file__).read_bytes()).hexdigest())
    (ROOT/'verification/correctness-cpp-integration-mocked-contracts.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,sort_keys=True))


if __name__=='__main__':
    main()
