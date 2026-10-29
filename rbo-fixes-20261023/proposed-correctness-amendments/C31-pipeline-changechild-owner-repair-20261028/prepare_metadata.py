"""Source-only repair of rvalue ownership required by ChangeChild."""
import copy,difflib,hashlib,json,subprocess,tempfile
from pathlib import Path
from types import ModuleType
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];PROJECT=ROOT.parent
REVISION='24e1aae9a0677e8ed12f257a7944c50516e9cba4'
EXPECTED={'c31_shared_pipeline_cpp_generator.py':'7237a294c0a31eb9da9ed6cd8069b643fa00dd38690ace3ec031d13938c2c4d6','c31_combined_cpp_generator.py':'7d56fc9bcc877762fc09238f46fa39983e0634e0f89fca9a183860b3fe57455e','correctness_cpp_profiles.json':'3d988379b86534a2f5fe5269d7b6dd957e7427ff5ea55c7a37f3a1b12112b679'}
REPLACEMENTS=[('ChangeChild(*branch->Child(1), 0, foreign)','ChangeChild(*branch->Child(1), 0, TExprNode::TPtr(foreign))'),('ChangeChild(*branch, 1, wrongMapInput)','ChangeChild(*branch, 1, std::move(wrongMapInput))'),('ChangeChild(*fields, 0, duplicateField)','ChangeChild(*fields, 0, std::move(duplicateField))'),('ChangeChild(*lambda, 1, wrongFields)','ChangeChild(*lambda, 1, std::move(wrongFields))'),('ChangeChild(*narrow, 1, wrongLambda)','ChangeChild(*narrow, 1, std::move(wrongLambda))'),('ChangeChild(*output, 0, wrongNarrow)','ChangeChild(*output, 0, std::move(wrongNarrow))')]
def sha(raw):return hashlib.sha256(raw).hexdigest()
def load(raw,path):
 m=ModuleType(path.stem);m.__file__=str(path);exec(compile(raw,str(path),'exec'),m.__dict__);return m

def main():
 checks=[]
 def check(ok,label):
  if not ok:raise AssertionError(label)
  checks.append(label)
 before={name:(ROOT/name).read_bytes() for name in EXPECTED}
 for name,expected in EXPECTED.items():check(sha(before[name])==expected,'exact_before_'+name)
 identity=(ROOT/'c31_identity_cpp_generator.py').read_bytes();check(sha(identity)=='112ca72315a8807599d960fe98e80d3b1128b37be9967ad17ff934f02370b8f9','original33_byte_identical')
 source=before['c31_shared_pipeline_cpp_generator.py'].decode();changed=source
 for idx,(old,new) in enumerate(REPLACEMENTS):check(changed.count(old)==1,'single_repair_location_'+str(idx));changed=changed.replace(old,new,1)
 reversed_source=changed
 for old,new in REPLACEMENTS:reversed_source=reversed_source.replace(new,old,1)
 check(reversed_source==source,'only_six_owner_expressions_changed')
 component=load(changed.encode(),HERE/'c31_shared_pipeline_cpp_generator.py');old_component=load(before['c31_shared_pipeline_cpp_generator.py'],ROOT/'c31_shared_pipeline_cpp_generator.py');cpp=component.render_cpp_suite();old_cpp=old_component.render_cpp_suite();expected_cpp=old_cpp
 for old,new in REPLACEMENTS:expected_cpp=expected_cpp.replace(old,new,1)
 check(cpp==expected_cpp,'rendered_change_only_six_owner_expressions')
 check(component.cases()==old_component.cases() and component.specs()==old_component.specs(),'four_cases_and_checkpoint_counts_preserved')
 for token in ('Checkpoint(', 'Require(', 'RequireRejected(', 'TInlineJoinFiltersRule','TGlobalPruningStage','TGlobalInliningStage','TAssignStagesStage','ComputePlanLiveness','TransformStageOutput','TPhysicalAggregationBuilder','ChangeChild('):check(cpp.count(token)==old_cpp.count(token),'contract_call_count_'+token)
 check('TExprNode::TPtr(foreign)' in changed and 'std::move(foreign)' not in changed,'foreign_owner_retained_for_later_negative_controls')
 check(changed.count('foreign.Get()')==source.count('foreign.Get()'),'foreign_identity_negatives_unchanged')
 for name in ('wrongMapInput','duplicateField','wrongFields','wrongLambda','wrongNarrow'):
  check(changed.count('std::move('+name+')')==1,'single_use_owner_consumed_'+name)
 combined=before['c31_combined_cpp_generator.py'].decode().replace(EXPECTED['c31_shared_pipeline_cpp_generator.py'],sha(changed.encode()),1).replace(sha(old_cpp.encode()),sha(cpp.encode()),1).encode()
 after={'c31_shared_pipeline_cpp_generator.py':changed.encode(),'c31_combined_cpp_generator.py':combined}
 with tempfile.TemporaryDirectory(prefix='c31-owner-metadata-') as td:
  tmp=Path(td);(tmp/'c31_identity_cpp_generator.py').write_bytes(identity)
  for name,raw in after.items():(tmp/name).write_bytes(raw)
  wrapper=load(combined,tmp/'c31_combined_cpp_generator.py');composed=wrapper.render_cpp_suite();vector=wrapper.specs();previous=load(before['c31_combined_cpp_generator.py'],ROOT/'c31_combined_cpp_generator.py');old_combined=previous.render_cpp_suite();expected_combined=old_combined
  for old,new in REPLACEMENTS:expected_combined=expected_combined.replace(old,new,1)
  check(composed==expected_combined,'combined_change_only_six_owner_expressions')
  check(vector==previous.specs(),'exact37_vector_and_assertion_counts')
  check(len(vector)==37 and sum(x['assertion_count'] for x in vector)==1442,'combined37_1442_checkpoints')
  check(sum(x['assertion_count'] for x in vector[:33])==1416,'old33_1416_checkpoints')
  check([x['assertion_count'] for x in vector[33:]]==[8,8,8,2],'four_exact8_8_8_2_checkpoints')
  for idx,(left,right) in enumerate(zip(vector,previous.specs())):check(left==right,'case_contract_'+str(idx))
  profile=json.loads(before['correctness_cpp_profiles.json']);old_profile=copy.deepcopy(profile);p=profile['profiles']['C31'];p.update(generator_sha256=sha(combined),generated_source_sha256=sha(composed.encode()),generated_source_bytes=len(composed.encode()))
  manifest=wrapper.vector_manifest()
  for key,value in list(p.items()):
   if isinstance(value,list) and value and isinstance(value[0],dict) and value[0].get('vector')=='identity' and 'generator_sha256' in value[0]:p[key]=manifest
  check(p['case_assertions']==old_profile['profiles']['C31']['case_assertions'],'profile_exact37_assertions')
  check(profile['profiles']['C47']==old_profile['profiles']['C47'],'C47_profile_unchanged')
  after['correctness_cpp_profiles.json']=(json.dumps(profile,indent=2)+'\n').encode()
 api_path='yql/essentials/ast/yql_expr.h';api=subprocess.check_output(['git','show',REVISION+':'+api_path],cwd=PROJECT)
 check(b'ChangeChild(const TExprNode& node, ui32 index, TExprNode::TPtr&& child)' in api,'actual_api_requires_rvalue_owner')
 check(b'TPtr ChildPtr(ui32 index) const' in api,'ChildPtr_returns_fresh_owner_by_value')
 check('fields->Child(1)->ChildPtr(0)' in changed,'already_rvalue_ChildPtr_call_unchanged')
 check("'iostream', 'stdexcept', 'utility'" in source,'utility_header_already_present')
 locations=json.loads((ROOT/'verification/C31-compile-location-classification-1793156581368918049.json').read_bytes())['captures'][0]['error_locations']
 source_lines=source.splitlines();render_lines=old_combined.splitlines();location_bindings=[]
 for row in locations:
  line=render_lines[row['line']-1];matches=[i+1 for i,s in enumerate(source_lines) if s==line]
  check(len(matches)==1,'diagnostic_maps_uniquely_'+str(row['line']))
  check(any(old in line for old,new in REPLACEMENTS),'diagnostic_maps_to_owner_repair_'+str(row['line']))
  location_bindings.append(dict(row,generator_source_line=matches[0],generated_line_sha256=sha(line.encode())))
 parts=[]
 for name,raw in after.items():
  (HERE/name).write_bytes(raw);target=ROOT.name+'/'+name;parts.append('diff --git a/'+target+' b/'+target+'\n'+''.join(difflib.unified_diff(before[name].decode().splitlines(True),raw.decode().splitlines(True),fromfile='a/'+target,tofile='b/'+target)))
 patch=ROOT/'proposed-correctness-amendments/C31-pipeline-changechild-owner-repair-20261028.patch';patch.write_text(''.join(parts))
 with tempfile.TemporaryDirectory(prefix='c31-owner-apply-') as td:
  for name,raw in before.items():p=Path(td)/ROOT.name/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(raw)
  subprocess.run(['git','apply','--check',str(patch)],cwd=td,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE);subprocess.run(['git','apply',str(patch)],cwd=td,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  for name,raw in after.items():check((Path(td)/ROOT.name/name).read_bytes()==raw,'disposable_apply_'+name)
 report={'schema':'c31_pipeline_changechild_owner_repair_v1','status':'dormant_source_checked_native_pending','revision':REVISION,'apply_after':'C31-pipeline-const-pointer-repair-20261028.patch','patch':str(patch.relative_to(ROOT)),'patch_sha256':sha(patch.read_bytes()),'before':{k:sha(v) for k,v in before.items()},'after':{k:sha(v) for k,v in after.items()},'api_source':{'path':api_path,'sha256':sha(api)},'diagnostic_bindings':location_bindings,'cause':'ChangeChild requires an rvalue intrusive-pointer owner. Two named lvalues fail first; four dependent calls use the same invalid argument form. Copy an explicit temporary owner for reused foreign node, and move five single-use intermediate owners. Node identities and negative-oracle mutations are preserved.','case_count':37,'old33_source_sha256':sha(identity),'old33_checkpoints':1416,'four_checkpoint_counts':[8,8,8,2],'total_checkpoints':1442,'component_manifest':manifest,'generated_source_sha256':sha(composed.encode()),'generated_source_bytes':len(composed.encode()),'checks_passed':len(checks),'checks':checks,'scope':{'production_or_live_helpers_edited':False,'native_build_toolchain_runs':False,'rendered_CPP_saved_or_shown':False,'current_two_diagnostics_attributed':True,'candidate_compile_verified':False,'coverage_or_admission_changed':False}}
 out=ROOT/'verification/C31-pipeline-changechild-owner-repair-source-preparation-20261028.json';out.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n');print(json.dumps({'checks':len(checks),'patch_sha256':sha(patch.read_bytes()),'report_sha256':sha(out.read_bytes()),'after':report['after']}))
if __name__=='__main__':main()
