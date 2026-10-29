"""Metadata/mocked-file checks only; never runs a compiler or database."""
import ast
import copy
import hashlib
import importlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
REPO = ROOT.parent


def main():
    checks = []
    def check(value, name):
        if not value:
            raise AssertionError(name)
        checks.append(name)
    sha = lambda raw: hashlib.sha256(raw).hexdigest()
    composition = json.loads((ROOT / 'verification/quality-idle53-source-composition-20261028.json').read_bytes())
    with tempfile.TemporaryDirectory(prefix='q01-concrete-metadata-') as td:
        temp = Path(td).resolve()
        for folder in (ROOT, REPO / 'rbo-review-20261021'):
            for path in folder.iterdir():
                if path.is_file() and path.suffix in ('.py', '.json', '.conf'):
                    target = temp / path.relative_to(REPO)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(path.read_bytes())
        before = json.loads((HERE/'before-images.json').read_bytes())
        # Exact Q01 predecessor files suffice. Later reviewed C31-only helper
        # repairs do not alter this patch or any of its28 spill bindings.
        final_installed = all((temp/ROOT.name/name).exists() and sha((temp/ROOT.name/name).read_bytes()) == value
                              for name, value in before.items())
        if not final_installed:
            first = composition['ordered_patches'][0]
            if all((temp/name).exists() and sha((temp/name).read_bytes()) == value
                   for name, value in first['after_images'].items()):
                subprocess.run(['git', 'apply', '--reverse', str(ROOT/first['path'])], cwd=temp,
                               check=True, capture_output=True)
        for row in composition['ordered_patches']:
            check(sha((ROOT/row['path']).read_bytes()) == row['sha256'], 'frozen_predecessor')
            if not final_installed:
                subprocess.run(['git', 'apply', str(ROOT/row['path'])], cwd=temp, check=True, capture_output=True)
        check(all((temp/ROOT.name/name).exists() and sha((temp/ROOT.name/name).read_bytes()) == value
                  for name, value in before.items()), 'exact_Q01_per_file_predecessors')
        installed = temp / ROOT.name
        before = json.loads((HERE/'before-images.json').read_bytes())
        old_sources = {name: (installed/name).read_bytes() for name in before}
        for name, value in before.items():
            check(sha(old_sources[name]) == value, 'exact_predecessor_' + name)
        for path in HERE.glob('*.py'):
            if path.name != 'verify_metadata.py':
                ast.parse(path.read_bytes())
                (installed/path.name).write_bytes(path.read_bytes())
        (installed/'verification').mkdir(exist_ok=True)
        design = 'Q01-resource-scaling-admission-plan-20261027.json'
        (installed/'verification'/design).write_bytes((ROOT/'verification'/design).read_bytes())
        sys.path[:0] = [str(installed), str(temp/'rbo-review-20261021')]
        scaling = importlib.import_module('quality_q01_scaling_checks')
        paths = importlib.import_module('quality_q01_path_checks')
        contract = importlib.import_module('quality_q01_concrete_contract')
        runtime = importlib.import_module('quality_range_runtime_cpp_generator')
        runtime_contract = importlib.import_module('quality_range_runtime_contract')
        recipes = importlib.import_module('q01_scaling_recipe_metadata_contracts')
        for module in (scaling, paths, contract, runtime, runtime_contract, recipes):
            check(Path(module.__file__).resolve().is_relative_to(temp), 'temporary_module_' + module.__name__)
        def spill_bindings(raw):
            tree=ast.parse(raw)
            return ast.literal_eval(next(n.value for n in tree.body if isinstance(n,ast.Assign)
                and any(isinstance(t,ast.Name) and t.id=='_SOURCE_BINDINGS' for t in n.targets)))
        old_bindings=spill_bindings(old_sources['quality_spill_bounds.py'])
        new_bindings=spill_bindings((installed/'quality_spill_bounds.py').read_bytes())
        expected_changed={ROOT.name+'/'+name for name in ('quality_range_runtime_cpp_generator.py','quality_range_runtime_contract.py')}
        check(len(new_bindings)==len(old_bindings)==28 and set(new_bindings)==set(old_bindings),'spill_exact28_dependencies_preserved')
        check({k for k in old_bindings if old_bindings[k]!=new_bindings[k]}==expected_changed,'spill_only_two_necessary_bindings_changed')
        for name,value in new_bindings.items():
            check(sha((temp/name).read_bytes())==value,'spill_composed_dependency_exact_'+name)
        normalized=(installed/'quality_spill_bounds.py').read_text()
        for key in expected_changed: normalized=normalized.replace(new_bindings[key],old_bindings[key])
        check(normalized.encode()==old_sources['quality_spill_bounds.py'],'spill_policy_unchanged')
        recipe_checks = recipes.run_contract_checks()
        check(recipe_checks['assertions'] >= 1373, 'old_recipe_obligations_retained')
        old_scaling = {'__file__': str(installed/'quality_q01_scaling_checks.py')}
        exec(compile(old_sources['quality_q01_scaling_checks.py'], '<old-scaling-source>', 'exec'), old_scaling)
        check(len(old_scaling['specifications']()) == 16, 'original16_raw_profiles_retained')
        for case in old_scaling['specifications']():
            for rows in (2048, 4096):
                check(scaling.specifications()[case] == old_scaling['specifications']()[case], 'old_raw_case_spec_unchanged')
                check(scaling.recipes(rows)[case] == old_scaling['recipes'](rows)[case], 'old_raw_recipe_unchanged_in_RAM')
                check(scaling.schema_and_seeds(rows, case) == old_scaling['schema_and_seeds'](rows, case), 'old_raw_setup_unchanged_in_RAM')
                check(scaling.expected_rows(case, rows) == old_scaling['expected_rows'](case, rows), 'old_raw_oracle_unchanged_in_RAM')
        old_runtime = {}
        exec(compile(old_sources['quality_range_runtime_cpp_generator.py'], '<old-runtime-source>', 'exec'), old_runtime)
        check(runtime.specs() == old_runtime['specs'](), 'direct42_vector_unchanged')
        check(len(runtime.specs()) == 42, 'direct42_count')
        check(runtime.METRIC_FIELDS[:len(old_runtime['METRIC_FIELDS'])] == old_runtime['METRIC_FIELDS'], 'old_numeric_fields_retained')
        check(len(runtime.METRIC_FIELDS) == len(old_runtime['METRIC_FIELDS']) + 15, 'absolute_snapshot_fields15')
        check(runtime.PRELUDE.count('stats.Check(') == old_runtime['PRELUDE'].count('stats.Check('), 'all_old_native_checks_retained')
        source = runtime.render_cpp_suite()
        check(source.count('int main()') == 1 and source.count('allocator_used_before') == 1, 'anonymous_render_valid_metadata')
        del source
        runtime_records = []
        for index, spec in enumerate(runtime.specs()):
            cycles = 4 if spec['lifecycle'] == 'reset' else 1
            complete = spec['lifecycle'] in ('complete', 'yield', 'reset')
            limit = spec['lifecycle'] == 'limit'
            yielded = spec['lifecycle'] in ('yield', 'cancel_yield')
            record = dict.fromkeys(runtime.METRIC_FIELDS, 0)
            record.update(case_index=index, checks_executed=1, passed=True, cycles=cycles,
                operator_instances=1, cleanup_live_rows=0, limit_rejections=int(limit),
                output_rows=spec['rows']*cycles if complete else 7 if spec['lifecycle']=='cancel_output' else 0,
                input_rows=spec['rows']*cycles if complete else spec.get('limit_at',1) if limit else 7,
                input_yields=int(yielded), output_yields=int(yielded), peak_live_rows=int(spec['rows']>0),
                allocator_allocations=2, allocator_offloads=int(spec['rows']>0 and not limit),
                allocator_offloaded_bytes=32*int(spec['rows']>0 and not limit),
                peak_used_delta=4096, peak_allocated_delta=4096,
                allocator_used_before=4096, allocator_used_after_release=4096,
                allocator_used_after_consumption=8192, allocator_peak_used_after_consumption=12288,
                allocator_peak_allocated_after_consumption=12288,
                allocator_peak_used_before=8192, allocator_peak_used_after=12288,
                allocator_peak_allocated_before=8192, allocator_peak_allocated_after=12288,
                allocator_allocations_before=4, allocator_allocations_after=6,
                allocator_offloads_before=2, allocator_offloaded_bytes_before=64)
            record['input_fetches'] = record['input_rows']+record['input_yields']
            record['allocator_offloads_after'] = 2+record['allocator_offloads']
            record['allocator_offloaded_bytes_after'] = 64+record['allocator_offloaded_bytes']
            check(runtime_contract._case_observations(record,spec), 'all42_additive_snapshot_positive')
            runtime_records.append(record)
        parse_runtime = lambda records: runtime_contract.parse_verdicts(
            b'\n'.join(json.dumps(x).encode() for x in records), 42, 0)
        check(len(parse_runtime(runtime_records)) == 42, 'strict42_snapshot_parser')
        for index in range(42):
            for key in runtime.METRIC_FIELDS[-15:]:
                bad = copy.deepcopy(runtime_records); bad[index][key] = True
                try: parse_runtime(bad)
                except runtime_contract.MetadataError: check(True,'snapshot_boolean_rejected')
                else: check(False,'snapshot_boolean_rejected')
            for key,value in [('allocator_allocations_after',0),('allocator_offloads_after',0),
                              ('allocator_offloaded_bytes_after',0),('allocator_peak_used_after',0),
                              ('allocator_peak_allocated_after',0),('allocator_used_after_release',16384)]:
                bad = copy.deepcopy(runtime_records);bad[index][key]=value
                try: parse_runtime(bad)
                except runtime_contract.MetadataError: check(True,'snapshot_inconsistent_delta_rejected')
                else: check(False,'snapshot_inconsistent_delta_rejected')
        # Generic in-memory graph builders exercise the local parser contract.
        # They are not rendered plans or runnable workload reproductions.
        def graph(spec, phase):
            bounds = ['AsStruct', ['Bounds', ['parameters']]]
            queue = ['QueueCreate', ['row_type'], ['Void'], ['Uint64', '0']]
            source = ['FromFlow', ['input_stream']]
            collector = ['WinFramesCollector', source, queue, bounds]
            root = collector
            if spec['family'] == 'setup_scan':
                root = ['input_stream']
            elif phase == 'fixed' and spec['kind'] == 'range' and spec['minmax']:
                minimum = ['KqpRangeMinMax', source, bounds,
                           [['input_value', 'minimum_value', 'min'], ['input_value', 'maximum_value', 'max']]]
                root = minimum
                if spec['collectors'] or spec.get('fallback'):
                    collector[1] = minimum
                    root = collector
                root = ['ExpandMap', root, ['lambda', ['$row'],
                    ['Member', '$row', 'minimum_value'], ['Member', '$row', 'maximum_value']]]
            elif phase == 'fixed' and spec['minmax']:
                def make(update):
                    folds = []
                    for _ in range(2):
                        frame = ['WinFrame', queue, ['Uint64', '0'], ['Bool', 'false'],
                                 ['Bool', 'false'], ['Bool', 'false'], ['DependsOn', '$row']]
                        entering = ['If', ['initial_frame'], frame, ['last_only']] if update else frame
                        push = ['lambda', ['$item', '$state'], ['Prepend', '$item', '$state']]
                        folds += [['Fold', entering, [], push], ['Fold', ['Nth', ['$previous'], '1'], [], push]]
                    return ['lambda', ['$row','$previous'] if update else ['$row'], folds]
                root = ['Chain1Map', collector, make(False), make(True)]
            if spec['family'] != 'setup_scan' and (phase == 'baseline' or spec['collectors'] or spec['kind'] == 'rows'):
                frame = ['WinFrame', queue, ['Uint64', '0'], ['Bool', 'false'],
                         ['Bool', 'false'], ['Bool', 'false'], ['DependsOn', '$row']]
                frame_use = ['Fold1', frame, ['init'], ['update']] if phase == 'baseline' or spec.get('fallback') else ['Head', frame]
                root = ['Map', root, ['lambda', ['$row'], frame_use]]
            if phase == 'fixed' and spec['collectors']:
                output = []
                for label in ('prefix_count_all','prefix_count_value','prefix_sum_value'):
                    frame = ['WinFrame',queue,['Uint64','0'],['Bool','false'],['Bool','false'],['Bool','false'],['DependsOn','$row']]
                    last = list(frame);last[5] = ['Bool','true']
                    calculation = ['+', ['-', ['Member','$last',label], ['Member','$first',label]], ['first_contribution']]
                    output.append(['IfPresent',['Head',frame],last,['lambda',['$first','$last'],calculation],['zero']])
                root = ['ExpandMap',root,['lambda',['$row'],*output]]
            nodes, seen, todo = [], set(), [root]
            while todo:
                node = todo.pop()
                if not isinstance(node, list) or id(node) in seen:
                    continue
                seen.add(id(node)); nodes.append(node)
                todo.extend(node)
            return lambda node: node, nodes

        witnesses = {}
        for case in scaling.finite_profile_cases():
            spec = scaling.specifications()[case]
            for phase in ('baseline', 'fixed'):
                resolver, nodes = graph(spec, phase)
                with patch.object(paths.windows, 'parse_physical_graph', return_value=(resolver, nodes)):
                    witness = paths.observe(spec, 'metadata-only-opaque-input')
                check(witness['path_pass'] is True, 'positive_local_graph_' + case + '_' + phase)
                dummy = {'q01_path_witness': witness,
                         'physical_ast': {'physical_ast_sha256': witness['physical_ast_sha256']}}
                contract._path(dummy, spec, phase)
                check(True, 'closed_route_' + case + '_' + phase)
                witnesses[(case, phase)] = witness
                for key in witness['checks']:
                    bad = copy.deepcopy(dummy)
                    bad['q01_path_witness']['checks'][key] = False
                    try:
                        contract._path(bad, spec, phase)
                    except ValueError:
                        check(True, 'failed_shape_flag_rejected')
                    else:
                        check(False, 'failed_shape_flag_rejected')
                if phase == 'fixed' and spec['kind'] == 'range' and spec['minmax']:
                    for fault in ('wrong_member_owner', 'duplicate_mapping', 'foreign_bounds', 'split_stream'):
                        resolver, nodes = graph(spec, phase)
                        op = next(n for n in nodes if n and n[0] == 'KqpRangeMinMax')
                        if fault == 'duplicate_mapping':
                            op[3][1][1] = op[3][0][1]
                        elif fault == 'wrong_member_owner':
                            for n in nodes:
                                if n and n[0] == 'Member': n[1] = '$foreign'
                        elif spec['collectors']:
                            outer = next(n for n in nodes if n and n[0] == 'WinFramesCollector')
                            if fault == 'foreign_bounds': outer[3] = copy.deepcopy(outer[3])
                            else: outer[1] = ['FromFlow', ['unrelated_input']]
                        else:
                            continue
                        with patch.object(paths.windows, 'parse_physical_graph', return_value=(resolver, nodes)):
                            check(paths.observe(spec, 'metadata-only-opaque-input')['path_pass'] is False,
                                  'unrelated_or_malformed_graph_rejected_' + fault)
                if phase=='fixed' and spec['collectors']:
                    for fault in ('wrong_endpoint_owner','wrong_endpoint_queue','not_last_row','duplicate_output_arithmetic'):
                        resolver,nodes=graph(spec,phase)
                        endpoints=[n for n in nodes if n and n[0]=='IfPresent' and len(n)==5]
                        if fault=='wrong_endpoint_owner':
                            for n in nodes:
                                if n and n[0]=='Member' and n[1]=='$last':n[1]='$unrelated'
                        elif fault=='wrong_endpoint_queue':
                            for n in endpoints:n[2][1]=['QueueCreate',['foreign_type']]
                        elif fault=='not_last_row':
                            for n in endpoints:n[2][5]=['Bool','false']
                        else:
                            for n in nodes:
                                if n and n[0]=='Member' and n[1] in ('$first','$last'):n[2]='same_output'
                        with patch.object(paths.windows,'parse_physical_graph',return_value=(resolver,nodes)):
                            check(paths.observe(spec,'metadata-only-opaque-input')['path_pass'] is False,'endpoint_path_rejected_'+fault)
                if phase=='fixed' and spec['kind']=='rows' and spec['minmax']:
                    resolver,nodes=graph(spec,phase)
                    chain=next(n for n in nodes if n and n[0]=='Chain1Map')
                    chain[3]=copy.deepcopy(chain[2])
                    with patch.object(paths.windows,'parse_physical_graph',return_value=(resolver,nodes)):
                        observed=paths.observe(spec,'metadata-only-opaque-input')
                    dummy={'q01_path_witness':observed,'physical_ast':{'physical_ast_sha256':observed['physical_ast_sha256']}}
                    try:contract._path(dummy,spec,'fixed')
                    except ValueError:check(True,'missing_update_entering_transfer_path_rejected')
                    else:check(False,'missing_update_entering_transfer_path_rejected')
        with patch.object(paths.windows, 'parse_physical_graph', side_effect=ValueError()):
            check(paths.observe(scaling.specifications()[scaling.finite_profile_cases()[0]], '')['complete'] is False,
                  'unknown_actual_plan_fails_closed')
        frozen = contract.manifest(2048, 'a'*32)
        check(len(frozen['profiles']) == 15 and len(frozen['schedule']) == 180, 'finite15_180_schedule')
        check(sum(s['kind'] == 'warmup' for s in frozen['schedule']) == 30, 'visible_warmups30')
        check(sum(s['kind'] == 'measurement' for s in frozen['schedule']) == 150, 'measured_attempts150')
        check(contract.validate_stage_A([], frozen)['semantic_path_pass'] is False, 'empty_vector_rejected')
        def bind_mock_sample(template, sample, ordinal):
            value = copy.deepcopy(template)
            memory_path = temp/('metadata-only-'+sample['kind']+'-memory-'+str(ordinal)+'.json')
            memory_path.write_bytes(Path(value['native_batch_memory_evidence']).read_bytes())
            value['native_batch_memory_evidence'] = str(memory_path)
            profile = next(p for p in frozen['profiles'] if p['case']==sample['case'])
            identity = dict(sample,run_id=frozen['run_id'],rows=frozen['rows'],mode='new',
                revision=contract.REVISIONS[sample['phase']],cell=profile['cell'],workload=profile['workload'],
                native_driver_sha256=sha(Path(contract.native.__file__).read_bytes()),
                dependency_source_sha256=scaling.source_bindings(),
                executable_sha256=value['binary_provenance']['sha256'],build_receipt_sha256=value['build_receipt_sha256'],
                resource_design_sha256=frozen['superseded_proposal_sha256'])
            bindings = dict(identity_sha256=sha(contract.encoded(identity)),timeout_seconds=120,
                inherited_environment_sha256='c'*64,python_executable_sha256='d'*64,udf_library_sha256='e'*64,
                inherited_scope_sha256=sha(str(ordinal).encode()),command_sha256='1'*64,linux_release='mock_linux',machine='mock_machine',
                cpu_affinity=[0],protocol_source_sha256=sha((installed/'anonymous_child_measurement.py').read_bytes()),
                integration_source_sha256=sha((installed/'q01_native_child_measurement.py').read_bytes()),
                outer_guard_source_sha256=sha((installed/'anonymous_cpp_guard.py').read_bytes()))
            raw = dict(schema_version='linux_wait4_one_child_v1',nonce=format(ordinal,'032x'),waited_pid=ordinal+1,
                wait_status=0,exit_code=0,termination_signal=None,core_dump_status_bit=False,guard_exit_code=0,
                user_cpu_seconds=1.0,system_cpu_seconds=0.25,linux_ru_maxrss_kib=1024)
            value['q01_measurement_identity'] = identity
            value['q01_child_observation'] = dict(schema_version='q01_inherited_scope_child_observation_v1',
                status='raw_observation',outer_memory_guard_verified=True,outer_memory_guard_admission_pending=False,
                outer_memory_guard_sha256=value['native_batch_memory_sha256'],target_group_empty=True,
                local_cleanup_complete=True,metric_scope='whole_kqprun_lifetime_including_startup_schema_fixtures_and_target',
                python_generation_and_result_oracle_included=False,bindings=bindings,raw_child_report=raw,
                report_sha256=sha(contract.encoded(raw)),linux_ru_maxrss_bytes=1024*1024)
            return value
        records = []
        for profile in frozen['profiles']:
            for phase in ('baseline', 'fixed'):
                case, revision = profile['case'], contract.REVISIONS[phase]
                memory_path = temp / (case + '-' + phase + '-memory.json')
                memory = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False, stdout_overflow=False,
                    max_scope_gib=24, min_host_available_gib=36, minimum_host_available_bytes=40*contract.GIB,
                    peak_scope_usage_bytes=1, peak_group_rss_bytes=1)
                memory_path.write_text(json.dumps(memory))
                worktree = temp / ('metadata-only-' + phase)
                binary = dict(resolved_path=str(worktree/'ydb/tests/tools/kqprun/kqprun'), sha256='b'*64,
                              size_bytes=1, mtime_ns=1, ctime_ns=1, inode=1, device=1)
                receipt = dict(schema_version='successful_runner_build_v1', revision=revision,
                    worktree=str(worktree), binary=binary, build=dict(exit_code=0, target='ydb/tests/tools/kqprun'))
                receipt_path = temp/(phase+'-receipt.json'); receipt_path.write_text(json.dumps(receipt))
                metadata = scaling.metadata(case, 2048)
                pragmas = dict(metadata['pragmas']); pragmas['YqlSelect'] = 'force'
                settings = contract.native.config_with((contract.native.REVIEW/'new.conf').read_text(), metadata['table_service_overrides'])
                record = dict(profile['workload'], case=case, phase=phase, revision=revision, mode='new', query_syntax='yql', rows=2048,
                    repetition=0, node_count=1, exit_code=0, generator_module='quality_q01_scaling_checks',
                    generator_version=scaling.GENERATOR_VERSION, generator_hash_source='loaded_source_snapshot',
                    quality_profile_sha256=frozen['profile_sha256'],
                    verification_contract_version='strict_results_and_request_boundaries_v1',
                    row_count=2048,result_set_count=1,result_set_boundaries_known=True,
                    setup_request_count=1,target_request_count=1,validation_request_count=0,
                    target_results_checked=True,expected_rejection_configured=False,result_sha256='d'*64,
                    semantic_oracle_pass=True, binary_provenance_verified=True, native_batch_guard_pass=True,
                    native_batch_memory_evidence=str(memory_path), native_batch_memory_sha256=sha(memory_path.read_bytes()),
                    build_receipt_path=str(receipt_path), build_receipt_sha256=sha(receipt_path.read_bytes()), binary_provenance=binary,
                    q01_path_witness=witnesses[(case, phase)],
                    physical_ast={'physical_ast_sha256': witnesses[(case, phase)]['physical_ast_sha256']})
                record.update(contract.native.optimizer_mode_configuration(settings, 'new', 'yql', pragmas, False))
                records.append(record)
        records = [bind_mock_sample(value,dict(case=value['case'],phase=value['phase'],kind='pilot',index=0),i)
                   for i,value in enumerate(records)]
        verdict = contract.validate_stage_A(records, frozen)
        check(verdict['semantic_path_pass'] is True, 'complete_mock_path_vector_passes_only_stage_A')
        check(verdict['pilot_native_identity_and_protocol_pass'] is True
              and verdict['raw_measurement_vector_pass'] is False,
              'pilot_identity_protocol_required_without_measurement_admission')
        check(verdict['optimization_acceptance_pass'] is False and verdict['allocation_snapshots_pass'] is False,
              'mock_path_success_cannot_promote_quality_or_allocation')
        # Reuse the already independently constructed exact manifest only for
        # the many rejection probes. The real construction above still runs.
        def validate_mock(values, candidate=frozen, **options):
            def bound_manifest(rows, run_id):
                if rows != frozen['rows'] or run_id != frozen['run_id']:
                    raise ValueError('metadata_test_wrong_manifest_request')
                return frozen
            with patch.object(contract, 'manifest', side_effect=bound_manifest):
                return contract.validate_stage_A(values,candidate,**options)
        for index in range(30):
            for key,value in [('run_id','b'*32),('kind','measurement'),('index',1),
                              ('native_driver_sha256','0'*64),('dependency_source_sha256',{})]:
                bad=list(records);bad[index]=copy.deepcopy(records[index])
                identity=bad[index]['q01_measurement_identity'];identity[key]=value
                bad[index]['q01_child_observation']['bindings']['identity_sha256']=sha(contract.encoded(identity))
                check(validate_mock(bad)['semantic_path_pass'] is False,'each_pilot_native_identity_rejected')
            for field in ('q01_measurement_identity','q01_child_observation'):
                bad=list(records);bad[index]=copy.deepcopy(records[index]);del bad[index][field]
                check(validate_mock(bad)['semantic_path_pass'] is False,'each_pilot_missing_native_protocol_rejected')
        for key,value in [('local_cleanup_complete',False),('target_group_empty',False),
                          ('outer_memory_guard_verified',False),('outer_memory_guard_admission_pending',True),
                          ('outer_memory_guard_sha256','0'*64),('status','unavailable')]:
            bad=copy.deepcopy(records[:1])+records[1:];bad[0]['q01_child_observation'][key]=value
            check(validate_mock(bad)['semantic_path_pass'] is False,'pilot_child_or_guard_failure_rejected')
        for section,key in [('bindings','inherited_scope_sha256'),('raw_child_report','nonce')]:
            bad=copy.deepcopy(records[:2])+records[2:]
            bad[1]['q01_child_observation'][section][key]=bad[0]['q01_child_observation'][section][key]
            if section=='raw_child_report':
                bad[1]['q01_child_observation']['report_sha256']=sha(contract.encoded(bad[1]['q01_child_observation'][section]))
            check(validate_mock(bad)['semantic_path_pass'] is False,'pilot_reused_native_scope_or_child_rejected')
        bad=copy.deepcopy(records[:2])+records[2:]
        bad[1]['native_batch_memory_evidence']=bad[0]['native_batch_memory_evidence']
        bad[1]['native_batch_memory_sha256']=bad[0]['native_batch_memory_sha256']
        check(validate_mock(bad)['semantic_path_pass'] is False,'pilot_reused_single_cell_guard_rejected')
        for index in range(len(records)):
            for key, value in [('semantic_oracle_pass', False), ('exit_code', True), ('rows', True),
                               ('phase', 'other'), ('query_sha256', '0'*64), ('generator_hash_source', 'other')]:
                bad = copy.deepcopy(records); bad[index][key] = value
                check(validate_mock(bad)['semantic_path_pass'] is False, 'record_mutation_rejected')
        for bad in (records[:-1], records+[records[0]], records[:-1]+[records[0]], list(reversed(records))):
            check(validate_mock(bad)['semantic_path_pass'] is False, 'incomplete_duplicate_or_reordered_vector_rejected')
        for key in ('profiles', 'schedule', 'profile_sha256', 'contract_sha256'):
            bad = copy.deepcopy(frozen); bad[key] = [] if isinstance(bad[key], list) else '0'*64
            check(validate_mock(records,bad)['semantic_path_pass'] is False, 'manifest_mutation_rejected')
        samples = []
        templates = {(r['case'],r['phase']):r for r in records}
        for ordinal,sample in enumerate(frozen['schedule']):
            samples.append(bind_mock_sample(templates[(sample['case'],sample['phase'])],sample,ordinal))
        verdict = validate_mock(samples,kind='measurement')
        check(verdict['raw_measurement_vector_pass'] is True and len(verdict['raw_metrics'])==180,
              'complete_mock180_raw_samples_pass_only_raw_vector')
        check(len(verdict['finite_raw_comparisons'])==15 and all(
            x['measured_samples_per_phase']==5 and x['fixed_all_total_CPU_below_baseline'] is False
            and x['baseline_all_total_CPU_below_fixed'] is False
            and x['observation_only_no_accepting_threshold'] is True for x in verdict['finite_raw_comparisons']),
            'equal_mock_samples_have_no_gain_claim')
        check(verdict['optimization_acceptance_pass'] is False
              and verdict['resource_scaling_verified'] is False
              and verdict['measured_contract_dispatch_binding_present'] is False,
              'mock_raw_success_never_grants_resource_or_optimization_acceptance')
        for index in range(180):
            value = copy.deepcopy(samples[index]); value['q01_measurement_identity']['index']=True
            bad = list(samples);bad[index]=value
            check(validate_mock(bad,kind='measurement')['raw_measurement_vector_pass'] is False,
                  'each_sample_wrong_coordinates_rejected')
        for target,key,value in [('identity','cell',{}),('identity','resource_design_sha256','0'*64),
                ('identity','executable_sha256','0'*64),('identity','dependency_source_sha256',{}),
                ('observation','status','unavailable'),('observation','local_cleanup_complete',False),
                ('observation','target_group_empty',False),('observation','outer_memory_guard_sha256','0'*64),
                ('observation','metric_scope','query_only'),('observation','python_generation_and_result_oracle_included',True),
                ('observation','linux_ru_maxrss_bytes',True),('bindings','identity_sha256','0'*64),
                ('bindings','timeout_seconds',True),('bindings','cpu_affinity',[1,0]),
                ('bindings','inherited_environment_sha256','0'*64),('bindings','protocol_source_sha256','0'*64),
                ('raw','user_cpu_seconds',float('inf')),('raw','system_cpu_seconds',-1),('raw','wait_status',True),
                ('raw','linux_ru_maxrss_kib',True),('raw','termination_signal',9),('raw','nonce',''),('raw','extra',0)]:
            bad = copy.deepcopy(samples[:1])+samples[1:]
            where = {'identity':bad[0]['q01_measurement_identity'],'observation':bad[0]['q01_child_observation'],
                'bindings':bad[0]['q01_child_observation']['bindings'],'raw':bad[0]['q01_child_observation']['raw_child_report']}[target]
            where[key] = value
            if target=='raw' and value != float('inf'):
                bad[0]['q01_child_observation']['report_sha256']=sha(contract.encoded(where))
            check(validate_mock(bad,kind='measurement')['raw_measurement_vector_pass'] is False,
                  'raw_binding_scope_or_numeric_mutation_rejected')
        for bad in (samples[:-1],samples+[samples[0]],samples[:-1]+[samples[0]],list(reversed(samples))):
            check(validate_mock(bad,kind='measurement')['raw_measurement_vector_pass'] is False,
                  'incomplete_extra_duplicate_reordered_raw_vector_rejected')
        check(validate_mock(samples,kind='measurement',runtime_evidence={})['allocation_snapshots_pass'] is False,
              'unverified_allocator_evidence_rejected')
        for section,key in [('raw_child_report','nonce'),('bindings','inherited_scope_sha256')]:
            bad=copy.deepcopy(samples[:2])+samples[2:]
            bad[1]['q01_child_observation'][section][key]=bad[0]['q01_child_observation'][section][key]
            if section=='raw_child_report':
                bad[1]['q01_child_observation']['report_sha256']=sha(contract.encoded(bad[1]['q01_child_observation'][section]))
            check(validate_mock(bad,kind='measurement')['raw_measurement_vector_pass'] is False,'reused_scope_or_child_record_rejected')
        for key in ('row_count','result_set_count','result_set_boundaries_known','setup_request_count',
                    'target_request_count','validation_request_count','target_results_checked','expected_rejection_configured','result_sha256'):
            bad=copy.deepcopy(records[:1])+records[1:];del bad[0][key]
            check(validate_mock(bad)['semantic_path_pass'] is False,'missing_request_or_result_boundary_rejected')
        canonical_counts={}
        for name in ('quality_window_checks.py','quality_sum_checks.py','quality_minmax_checks.py','quality_range_minmax_checks.py'):
            check((installed/name).read_bytes() == (ROOT/name).read_bytes(), 'canonical_generator_unchanged_' + name)
            canonical_counts[name]=len([c for c in importlib.import_module(name[:-3]).recipes(32) if c.startswith('q01_')])
        check(list(canonical_counts.values())==[12,197,161,282] and sum(canonical_counts.values())==652,
              'canonical652_exact_current_owners_preserved')
        for name in ('quality_pairing.py','native_verify.py','anonymous_cpp_verify.py','anonymous_cpp_guard.py'):
            check(sha((installed/name).read_bytes()) == composition['after_images'].get(ROOT.name+'/'+name,
                  sha((ROOT/name).read_bytes())), 'live_admission_or_execution_helper_unchanged_' + name)
    report = {'schema':'q01_concrete_stage_A_metadata_checks_v1', 'checks_passed':len(checks), 'checks':checks,
              'existing_recipe_checks_passed':recipe_checks['assertions'], 'native_or_toolchain_runs':0,
              'raw_inputs_or_plans_persisted':False, 'mock_success_is_not_native_evidence':True,
              'optimization_acceptance_enabled':False}
    print(json.dumps(report, sort_keys=True))


if __name__ == '__main__':
    main()
