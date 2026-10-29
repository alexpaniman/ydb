"""Source-only Q28 depth proof and dormant four-control preparation."""
import ast,difflib,hashlib,json,subprocess,tempfile
from pathlib import Path
from types import ModuleType
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];PROJECT=ROOT.parent
REVISION='567d22ff15c8036cbc9741e0bb29daa6c140f0c1'
def sha(raw):return hashlib.sha256(raw).hexdigest()
def load(raw,path):
 m=ModuleType(path.stem);m.__file__=str(path);exec(compile(raw,str(path),'exec'),m.__dict__);return m

def main():
 checks=[]
 def check(ok,label):
  if not ok:raise AssertionError(label)
  checks.append(label)
 paths=['ydb/core/kqp/opt/rbo/kqp_rbo_rules.h','ydb/core/kqp/opt/rbo/rules/push_map_elements_through_input.cpp','ydb/core/kqp/opt/rbo/kqp_rbo.cpp','ydb/core/kqp/opt/rbo/kqp_rbo.h','ydb/core/kqp/opt/rbo/kqp_operator.cpp','ydb/core/kqp/opt/rbo/kqp_operator.h','ydb/core/kqp/opt/rbo/kqp_info_unit.h','ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h']
 images={p:subprocess.check_output(['git','show',REVISION+':'+p],cwd=PROJECT) for p in paths};texts={p:b.decode() for p,b in images.items()}
 header=texts[paths[0]];start=header.index('class TPushMapElementsIntoMapRule');stop=header.index('class TPushMapElementsThroughInputRule',start);segment=header[start:stop]
 old='std::optional<ui32> LocalRewriteDepth() const override { return 2; }';new='std::optional<ui32> LocalRewriteDepth() const override { return 1; }'
 check(segment.count(old)==1,'single_IntoMap_depth_declaration');after=header[:start]+segment.replace(old,new,1)+header[stop:]
 check(after.replace(new,old,1)==header,'production_change_only_depth_two_to_one')
 check(after[after.index('class TPushMapElementsThroughInputRule'):].startswith(header[stop:]),'ThroughInput_depth_two_unchanged')
 rules=texts[paths[1]];begin=rules.index('TIntrusivePtr<IOperator> TPushMapElementsIntoMapRule::SimpleMatchAndApply');end=rules.index('bool TPushMapElementsThroughInputRule::QuickMatch',begin);body=rules[begin:end]
 check('bottom.AddMapElement(id, element);' in body,'only_lower_Map_definition_mutation')
 check('return top.GetInput();' in body,'removed_upper_returns_lower_Map_at_depth_zero')
 check('return MakeIntrusive<TOpMap>(top.GetInput(), top.Pos, std::move(kept));' in body,'kept_upper_new_root_lower_Map_at_depth_one')
 check(body.count('AddMapElement')==1 and 'SetChild(' not in body and 'SetInput(' not in body,'lower_Map_input_edges_unchanged')
 check('const auto available = bottom.GetInput()->GetOutputIUs();' in body,'only_read_unchanged_input_outputs')
 operator=texts[paths[4]]
 check('MapElements.Add(output, std::move(element));\n    Props.OutputIUs.reset();' in operator,'mutated_Map_invalidates_own_output_cache')
 check('auto result = GetInput()->GetOutputIUs();' in operator,'Map_output_depends_on_child_output_and_own_definitions')
 scheduler=texts[paths[2]]
 check('if (depth)' in scheduler and 'depth - 1' in scheduler,'scheduler_refreshes_all_children_within_declared_depth')
 check('changed.Current->RecomputeLocalOutputIUs();' in scheduler,'local_refresh_uses_actual_virtual_producer')
 check('work.insert(work.end(), region.rbegin(), region.rend());' in scheduler,'same_region_requeued')
 check('op->GetOutputIUs() == previousOutputs' in scheduler,'matched_root_output_identity_guard_preserved')
 check('while (fired && numMatches < maxNumOfMatches)' in scheduler,'conservative_final_pass_unchanged')
 check('TOpUnionAll(TVector<TIntrusivePtr<IOperator>> inputs' in texts[paths[5]],'real_variadic_UnionAll_constructor')
 check('const TUnorderedIUs& GetOutputIUs() override { return Columns.Keys(); }' in texts[paths[5]],'UnionAll_output_mapping_does_not_require_child_cache_refresh')
 check('struct TUnionInputRow' in texts[paths[6]] and 'absl::InlinedVector<TInfoUnitId, 1> Inputs;' in texts[paths[6]],'real_union_input_row_api')
 generator=HERE/'quality_scheduler_depth_checks.py';raw=generator.read_bytes();original_raw=(ROOT/'quality_scheduler_cpp_checks.py').read_bytes()
 module=load(raw,generator);check(module.REQUIRED_ORIGINAL_SHA256==sha(original_raw),'original26_exact_source_dependency')
 original=load(original_raw,ROOT/'quality_scheduler_cpp_checks.py');original_render=original.render_cpp_suite()
 check(len(original.cases())==26,'original26_case_count')
 with tempfile.TemporaryDirectory(prefix='q28-depth-source-') as td:
  folder=Path(td);(folder/'quality_scheduler_cpp_checks.py').write_bytes(original_raw);(folder/generator.name).write_bytes(raw)
  m=load(raw,folder/generator.name);cpp=m.render_cpp_suite();metadata=m.metadata()
  check(cpp.startswith(original.COMMON),'existing_source_probe_and_independent_evaluator_unchanged')
  check(len(m.specs())==4 and len(set(m.cases()))==4,'exact_four_additional_cells')
  check([(x['kept'],x['fanout']) for x in m.specs()]==[(False,8),(False,32),(True,8),(True,32)],'exact_kept_removed_fanout_vector')
  check('TPushMapElementsIntoMapRule::SimpleMatchAndApply' not in cpp,'production_rewrite_implementation_not_copied')
  check('class TOldDepthIntoProbe: public TIntoProbe' in cpp,'same_actual_rule_for_old_depth_control')
  check('std::optional<ui32> LocalRewriteDepth() const override { return 2; }' in m.CONTRACT,'same_build_depth_two_override')
  check('ComputeOutputIUs() override { ++UnionRefreshes; }' in m.CONTRACT,'union_probe_counts_virtual_refresh')
  check(m.CONTRACT.index('NTests::AssertIdInvariants')<m.CONTRACT.index('return measured;'),'semantic_and_ID_checks_before_measurement_publish')
  check(m.CONTRACT.index('MeasureDepth(fanout, kept, true)')<m.CONTRACT.index('CurrentDepth = current; OldDepth = old;'),'both_controls_complete_before_publish')
  check(m.CONTRACT.index('CurrentDepth = current; OldDepth = old;')<m.CONTRACT.index('LocalRewriteDepth() == std::optional<ui32>{1}'),'baseline_cost_failure_preserves_completed_semantic_counters')
  for expression in ('current.Reads + (kept ? 0 : fanout)','current.Unions + (kept ? 1 : 0)','current.Visits + (kept ? 1 : fanout)','current.Attempts + (kept ? 1 : fanout)'):
   check(expression in m.CONTRACT,'exact_source_derived_delta_'+sha(expression.encode())[:12])
  check(metadata['full_q28_admission'] is False and metadata['query_semantics_verified'] is False,'no_admission_inflation')
  check(metadata['required_original_case_count']==26,'original26_still_required')
  check(metadata['before_revision_is_q28_implementation_not_pinned_main'] is True,'amendment_pair_distinct_from_pinned_main26')
  check(original.render_cpp_suite()==original_render and sha((ROOT/'quality_scheduler_cpp_checks.py').read_bytes())==module.REQUIRED_ORIGINAL_SHA256,'original26_render_and_source_unchanged')
  changed=(folder/'quality_scheduler_cpp_checks.py');changed.write_bytes(original_raw+b'\n')
  try:m.render_cpp_suite()
  except ValueError:check(True,'dependency_drift_rejected')
  else:check(False,'dependency_drift_rejected')
 production_patch=ROOT/'proposed-quality-amendments/Q28-map-fusion-refresh-depth-20261028.patch';production_patch.write_text('diff --git a/'+paths[0]+' b/'+paths[0]+'\n'+''.join(difflib.unified_diff(header.splitlines(True),after.splitlines(True),fromfile='a/'+paths[0],tofile='b/'+paths[0])))
 install=ROOT/'proposed-quality-amendments/Q28-map-fusion-refresh-depth-controls-20261028.patch';target=ROOT.name+'/'+generator.name;install.write_text('diff --git a/'+target+' b/'+target+'\nnew file mode 100644\n'+''.join(difflib.unified_diff([],raw.decode().splitlines(True),fromfile='/dev/null',tofile='b/'+target)))
 with tempfile.TemporaryDirectory(prefix='q28-depth-apply-') as td:
  p=Path(td)/paths[0];p.parent.mkdir(parents=True);p.write_text(header)
  for patch in (production_patch,install):subprocess.run(['git','apply','--check',str(patch)],cwd=td,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE);subprocess.run(['git','apply',str(patch)],cwd=td,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  check(p.read_text()==after,'production_disposable_application')
  check((Path(td)/target).read_bytes()==raw,'generator_disposable_application')
 report={'schema':'q28_map_fusion_depth_source_assessment_v1','revision_before_amendment':REVISION,'status':'dormant_source_checked_native_and_driver_integration_pending','production_patch':{'path':str(production_patch.relative_to(ROOT)),'sha256':sha(production_patch.read_bytes()),'target':paths[0],'before_sha256':sha(images[paths[0]]),'after_sha256':sha(after.encode()),'semantic_rule_body_changed':False},'generator_patch':{'path':str(install.relative_to(ROOT)),'sha256':sha(install.read_bytes()),'target':target,'after_sha256':sha(raw)},'source_bindings':{p:sha(b) for p,b in images.items()},'classification':'optimization-quality over-refresh only; no correctness defect established','proof':['IntoMap changes only the lower Map definitions and its output cache. It never mutates the lower input or descendant nodes.','If the upper Map disappears, the changed lower Map is replacement depth0. If the upper Map remains, a fresh upper Map is depth0 and changed lower Map depth1.','Depth1 therefore covers all changes and their child-first output refresh. Root output equality, owning worklist, exclusions, and mandatory conservative final pass remain unchanged.','Current depth2 safely over-approximates those mutations, but when the upper Map disappears it reaches every child of a wide unchanged UnionAll below the lower Map. These unchanged sources are refreshed and requeued unnecessarily.','When the upper Map remains, depth2 additionally refreshes/requeues the unchanged lower input. The amendment omits that work.','ThroughInput retains depth2: its kept upper Map, modified input, and new child Maps can occupy replacement depths0,1,2.'], 'controls':metadata,'expected_counter_delta_scope':{'removed_upper':'Depth2 minus depth1: fanout source refreshes, candidate visits and IntoMap QuickMatch attempts; union refresh count equal.','kept_upper':'Depth2 minus depth1: one union refresh, candidate visit and IntoMap QuickMatch attempt; source refresh count equal.','inference_not_native_measurement':True},'existing26':{'source_sha256':sha(original_raw),'rendered_sha256':sha(original_render.encode()),'source_changed':False,'required_separately':True},'verification_route':{'current_direct26_CLI_changed':False,'new_module_registered':False,'needed_next':'Bind four amendment-specific records to exact current-Q28 and amended-Q28 production closures via a separately reviewed narrow adapter; keep pinned-main26 pairing required. No broad baseline compatibility shim or admission substitution.','no_native_execution_done':True},'source_checks_passed':len(checks),'source_checks':checks,'limitations':['Candidate C++ has not compiled or executed; exact counter deltas require native validation.','Four probes isolate IntoMap; original26 still verify both Map-push rules and excluded effects.','No right-join normalization coverage or complete-Q28 acceptance claim is added.','This does not remove conservative traversal in unreviewed stages, global logical-boundary invalidation, output-set costs or trace serialization.','No optimizer-wide linearity or latency improvement is claimed.'],'production_live_helpers_Git_objects_modified':False,'generated_CPP_queries_rows_plans_persisted':False}
 out=ROOT/'verification/Q28-map-fusion-refresh-depth-source-preparation-20261028.json';out.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n');print(json.dumps({'checks':len(checks),'production_patch_sha256':sha(production_patch.read_bytes()),'generator_patch_sha256':sha(install.read_bytes()),'generator_sha256':sha(raw),'report_sha256':sha(out.read_bytes())}))
if __name__=='__main__':main()
