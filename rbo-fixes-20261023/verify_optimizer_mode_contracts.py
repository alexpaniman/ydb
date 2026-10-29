"""Exercise prospective optimizer-mode repair in RAM; no native launches."""
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent/'rbo-review-20261021'))
import prepare_optimizer_mode_verifier_patch as prepared
from optimizer_mode_contract import optimizer_mode_configuration, optimizer_mode_matches_record


def main():
    edits = prepared.render_files()
    modules = {}
    for name in ('verification_contract', 'native_verify', 'interval_sum_checks', 'interval_window_checks'):
        module = ModuleType(name)
        module.__file__ = str(ROOT/(name+'.py'))
        sys.modules[name] = module
        exec(compile(edits[name+'.py'][1], module.__file__, 'exec'), module.__dict__)
        module._verification_source_sha256 = hashlib.sha256(edits[name+'.py'][1].encode()).hexdigest()
        modules[name] = module
    native, verdict = modules['native_verify'], modules['verification_contract']
    import distribution_checks as distribution
    import window_checks as windows
    import write_checks as writes
    assertions = 0
    mocked_calls = 0
    def check(value):
        nonlocal assertions
        assert value
        assertions += 1
    for module in (modules['interval_sum_checks'], modules['interval_window_checks']):
        for case in module.specs():
            for mode in ('new', 'legacy'):
                metadata = native.module_metadata(module, case, mode, 8)
                check(metadata['table_service_config']['EnableNewRBO'] is (mode == 'new'))
                check(metadata['table_service_config'] == module.requirements(case, mode)['table_service_config'])
        # The new optional mode argument preserves existing callers' defaults.
        first = next(iter(module.specs()))
        check(module.metadata(first, 8) == module.metadata(first, 8, mode='new'))
    case = next(iter(windows.recipes(8)))
    for mode in ('new','legacy'):
        check(native.module_metadata(windows,case,mode,8)['table_service_config']['EnableNewRBO'] is (mode=='new'))

    def run(module, case, mode, expect_launch=True):
        nonlocal mocked_calls
        def response(command, **kwargs):
            nonlocal mocked_calls
            mocked_calls += 1
            check(expect_launch)
            config_path = command[command.index('-c')+1]
            config = os.pread(int(config_path.rsplit('/',1)[1]), 1024*1024, 0).decode()
            metadata=native.module_metadata(module,case,mode,8)
            fallback=metadata.get('table_service_overrides',metadata.get('table_service_config',{})).get('EnableFallbackToYqlOptimizer',False)
            configuration = optimizer_mode_configuration(config, mode, 'yql', {'YqlSelect':'force' if mode=='new' else 'disable'},fallback)
            check(configuration['optimizer_mode_config_verified'])
            target_path = [command[index+1] for index,value in enumerate(command) if value=='-p'][-1]
            target = os.pread(int(target_path.rsplit('/',1)[1]), 1024*1024, 0).decode()
            check(('PRAGMA YqlSelect='+native.g.quote('force' if mode=='new' else 'disable')+';') in target)
            result_path = command[command.index('--result-file')+1]
            result_rows = (module.expected_rows(case,8) if hasattr(module,'expected_rows') else
                           [dict(id=i,k=1,v=3,source=tag) for tag in (0,1) for i in range(1,9)])
            data = '\n'.join(json.dumps(row) for row in result_rows)
            os.write(int(result_path.rsplit('/',1)[1]), data.encode())
            return SimpleNamespace(returncode=0,stdout=b'',stderr=b'')
        count = mocked_calls
        with patch.object(native.subprocess,'run',response):
            record = native.run_generated(module,case,mode,8)
        check(mocked_calls-count == int(expect_launch))
        check(optimizer_mode_matches_record(record) is expect_launch)
        check(record.get('semantic_oracle_pass') is expect_launch)
        return record

    distribution_records = []
    for module,case in ((distribution,'c37_window_distribution'),
                        (distribution,'c38_full_distribution'),
                        (modules['interval_sum_checks'],next(iter(modules['interval_sum_checks'].specs()))),
                        (modules['interval_window_checks'],next(iter(modules['interval_window_checks'].specs())))):
        for mode in ('new','legacy'):
            record=run(module,case,mode)
            check(verdict.record_verdict(record)=='functional')
            if module is distribution:distribution_records.append(record)
    fallback_record=run(writes,'c44_sink_disabled_fallback','new')
    check(verdict.record_verdict(fallback_record)=='functional')
    check(fallback_record['optimizer_mode_configuration']['enable_fallback'] is True)
    check(fallback_record['optimizer_mode_configuration']['fallback_requested'] is True)
    check(fallback_record['base_table_oracle_pass'] and fallback_record['index_table_oracle_pass'])
    check(not optimizer_mode_matches_record(fallback_record,require_exclusive=True))

    # The original wrapper really supplies new-only configuration. It must be
    # rejected before subprocess.run, even though YqlSelect has been changed.
    for name in ('interval_sum_checks','interval_window_checks'):
        old=ModuleType(name)
        old.__file__=str(ROOT/(name+'.py'))
        exec(compile(edits[name+'.py'][0],old.__file__,'exec'),old.__dict__)
        record=run(old,next(iter(old.specs())),'legacy',expect_launch=False)
        check(record['harness_exception']=='OptimizerModeConfigurationMismatch')
        check(verdict.record_verdict(record)=='failure')

    for bug in ('C37','C38'):
        pair=[record for record in distribution_records if record['case'].startswith(bug.lower())]
        check(verdict.summarize_verification(pair,bug)==(True,'verified'))
        missing=copy.deepcopy(pair)
        for record in missing:
            record.pop('optimizer_mode_configuration');record.pop('optimizer_mode_config_verified')
        check(verdict.summarize_verification(missing,bug)==(False,'native_failed'))
        for field,value in (('requested_mode','new'),('enable_new_rbo',True),('enable_fallback',True),
                            ('settings_sha256','1'*64),('yql_select','force')):
            changed=copy.deepcopy(pair)
            legacy=next(record for record in changed if record['mode']=='legacy')
            legacy['optimizer_mode_configuration'][field]=value
            check(verdict.summarize_verification(changed,bug)==(False,'native_failed'))
    check(verdict.record_verdict({'exit_code':0,'semantic_oracle_pass':True})=='functional')
    settings=(native.REVIEW/'legacy.conf').read_text()
    for altered in (settings.replace('EnableNewRBO: false','EnableNewRBO: true'),
                    settings.replace('EnableNewRBO: false','EnableNewRBO: invalid'),
                    settings+'\nEnableNewRBO: true\n',
                    settings.replace('EnableFallbackToYqlOptimizer: false','EnableFallbackToYqlOptimizer: true')):
        check(not optimizer_mode_configuration(altered,'legacy','yql',{'YqlSelect':'disable'})['optimizer_mode_config_verified'])
    check(optimizer_mode_configuration(settings,'legacy','pg',{})['optimizer_mode_config_verified'])
    result=dict(status='mocked_mode_contracts_passed_native_unverified',assertions=assertions,
                mocked_run_generated_calls=mocked_calls,blocked_mismatched_legacy_calls=2,
                actual_process_launches=0,native_runs=0,compiler_runs=0,active_helpers_edited=False,
                patch_sha256=hashlib.sha256(prepared.render_patch().encode()).hexdigest(),
                helper_sha256=hashlib.sha256((ROOT/'optimizer_mode_contract.py').read_bytes()).hexdigest(),
                generated_workloads_saved=False,synthetic_evidence_saved=False)
    (ROOT/'verification/optimizer-mode-mocked-contracts.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,sort_keys=True))


if __name__=='__main__':
    main()
