"""Prepare the single constness correction; generated C++ remains in RAM."""
import ast,copy,difflib,hashlib,json,subprocess,tempfile
from pathlib import Path
from types import ModuleType
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
PROJECT=ROOT.parent
REVISION='24e1aae9a0677e8ed12f257a7944c50516e9cba4'
EXPECTED={'c31_shared_pipeline_cpp_generator.py':'e222345eb70b71b1de31f58e37b14ec444782b34c97331002fbcff2008c99187','c31_combined_cpp_generator.py':'605f1f1d2fd286af2fed2eeb39209c11ffa353ac88ecacac0a5ef9ac83833ad1','correctness_cpp_profiles.json':'102d95f24ee88e8b38d0e1e0ee005423d7c61fa4d4293325ac6ee3988eb0f08d'}
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
 identity=(ROOT/'c31_identity_cpp_generator.py').read_bytes();check(sha(identity)=='112ca72315a8807599d960fe98e80d3b1128b37be9967ad17ff934f02370b8f9','old33_source_unchanged')
 source=before['c31_shared_pipeline_cpp_generator.py'].decode();old='    auto* expand = combine->Child(0);';new='    const auto* expand = combine->Child(0);';check(source.count(old)==1,'single_mutable_local_pointer')
 changed=source.replace(old,new,1).encode();component=load(changed,HERE/'c31_shared_pipeline_cpp_generator.py');old_component=load(before['c31_shared_pipeline_cpp_generator.py'],ROOT/'c31_shared_pipeline_cpp_generator.py')
 old_cpp=old_component.render_cpp_suite();cpp=component.render_cpp_suite();check(cpp==old_cpp.replace(old,new,1),'generated_change_only_constness')
 check(component.cases()==old_component.cases(),'four_case_vector_preserved');check(component.specs()==old_component.specs(),'four_checkpoint_vector_preserved');check([x['assertion_count'] for x in component.specs()]==[8,8,8,2],'exact_four_checkpoint_counts')
 for token in ('Checkpoint(', 'Require(', 'RequireRejected(', 'TInlineJoinFiltersRule', 'TGlobalPruningStage','TGlobalInliningStage','TAssignStagesStage','ComputePlanLiveness','TransformStageOutput','TPhysicalAggregationBuilder'):
  check(cpp.count(token)==old_cpp.count(token),'unchanged_contract_calls_'+token)
 combined=before['c31_combined_cpp_generator.py'].decode();combined=combined.replace(EXPECTED['c31_shared_pipeline_cpp_generator.py'],sha(changed),1).replace(sha(old_cpp.encode()),sha(cpp.encode()),1).encode()
 after={'c31_shared_pipeline_cpp_generator.py':changed,'c31_combined_cpp_generator.py':combined}
 with tempfile.TemporaryDirectory(prefix='c31-constness-metadata-') as td:
  tmp=Path(td);(tmp/'c31_identity_cpp_generator.py').write_bytes(identity)
  for name,raw in after.items():(tmp/name).write_bytes(raw)
  wrapper=load(combined,tmp/'c31_combined_cpp_generator.py');composed=wrapper.render_cpp_suite();vector=wrapper.specs()
  old_wrapper=load(before['c31_combined_cpp_generator.py'],ROOT/'c31_combined_cpp_generator.py');old_composed=old_wrapper.render_cpp_suite()
  check(composed==old_composed.replace(old,new,1),'combined37_change_only_constness')
  check(wrapper.specs()==old_wrapper.specs(),'combined_exact_vector_preserved')
  check(len(vector)==37 and sum(x['assertion_count'] for x in vector)==1442,'combined37_exact_1442_counts')
  check(sum(x['assertion_count'] for x in vector[:33])==1416,'original33_exact1416_counts')
  for idx,(left,right) in enumerate(zip(vector,old_wrapper.specs())):check(left==right,'case_contract_'+str(idx))
  profile=json.loads(before['correctness_cpp_profiles.json']);original_profile=copy.deepcopy(profile);p=profile['profiles']['C31']
  p.update(generator_sha256=sha(combined),generated_source_sha256=sha(composed.encode()),generated_source_bytes=len(composed.encode()))
  # The optional component manifest is already part of the existing profile.
  for key in ('component_vectors','vectors','component_manifest'):
   if key in p:p[key]=wrapper.vector_manifest()
  # Use existing manifest key without inventing a new profile schema.
  for key,value in list(p.items()):
   if isinstance(value,list) and value and isinstance(value[0],dict) and value[0].get('vector')=='identity' and 'generator_sha256' in value[0]:p[key]=wrapper.vector_manifest()
  check(p['case_assertions']==original_profile['profiles']['C31']['case_assertions'],'profile_exact37_assertions')
  for key in p:
   if key not in ('generator_sha256','generated_source_sha256','generated_source_bytes') and p[key]!=original_profile['profiles']['C31'][key]:
    check(isinstance(p[key],list) and p[key]==wrapper.vector_manifest(),'only_component_hash_manifest_refreshed')
  check(profile['profiles']['C47']==original_profile['profiles']['C47'],'C47_profile_unchanged')
  after['correctness_cpp_profiles.json']=(json.dumps(profile,indent=2)+'\n').encode()
  manifest=wrapper.vector_manifest()
 api_paths=['yql/essentials/ast/yql_expr.h','ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_convertion_utils.h']
 api={p:subprocess.check_output(['git','show',REVISION+':'+p],cwd=PROJECT) for p in api_paths}
 check(b'TExprNode* Child(ui32 index) const' in api[api_paths[0]],'actual_Child_returns_mutable_pointer')
 check('static const TExprNode* Unary(' in source,'local_Unary_returns_const_pointer')
 check('if (blocks) expand = Unary(' in source,'block_unwrap_assigns_const_return')
 check('static const TExprNode* Unary(' in changed.decode(),'no_const_cast_or_API_weakening')
 parts=[]
 for name,raw in after.items():
  (HERE/name).write_bytes(raw);target=ROOT.name+'/'+name
  parts.append('diff --git a/'+target+' b/'+target+'\n'+''.join(difflib.unified_diff(before[name].decode().splitlines(True),raw.decode().splitlines(True),fromfile='a/'+target,tofile='b/'+target)))
 patch=ROOT/'proposed-correctness-amendments/C31-pipeline-const-pointer-repair-20261028.patch';patch.write_text(''.join(parts))
 with tempfile.TemporaryDirectory(prefix='c31-constness-apply-') as td:
  for name,raw in before.items():p=Path(td)/ROOT.name/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(raw)
  subprocess.run(['git','apply','--check',str(patch)],cwd=td,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  subprocess.run(['git','apply',str(patch)],cwd=td,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  for name,raw in after.items():check((Path(td)/ROOT.name/name).read_bytes()==raw,'disposable_apply_'+name)
 report={'schema':'c31_pipeline_const_pointer_repair_v1','status':'dormant_source_checked_compile_pending','revision':REVISION,'patch':str(patch.relative_to(ROOT)),'patch_sha256':sha(patch.read_bytes()),'before':{k:sha(v) for k,v in before.items()},'after':{k:sha(v) for k,v in after.items()},'source_api_bindings':{k:sha(v) for k,v in api.items()},'cause':'Child() returns TExprNode* even on const nodes; auto* inferred a mutable observer pointer, then block-unwrapping assigned Unary(), which returns const TExprNode*. Declare only that local observer pointer const.','diagnostic_evidence':{'path':'verification/C31-compile-diagnostic-classification-1793155174803712636.json','sha256':sha((ROOT/'verification/C31-compile-diagnostic-classification-1793155174803712636.json').read_bytes()),'source_matched_error':0,'remaining_two_diagnostics':'not attributed from sanitized hashes; may be secondary or independent; guarded recompile needed'},'case_count':37,'old_identity_count':33,'old_identity_assertion_count':1416,'pipeline_checkpoint_counts':[8,8,8,2],'combined_check_count':1442,'component_manifest':manifest,'generated_source_sha256':sha(composed.encode()),'generated_source_bytes':len(composed.encode()),'checks_passed':len(checks),'checks':checks,'scope':{'production_changes':False,'live_helper_edits':False,'native_build_toolchain_runs':False,'generated_cpp_persisted':False,'coverage_or_admission_changed':False,'old33_source_byte_identical':True,'C47_profile_changed':False}}
 dest=ROOT/'verification/C31-pipeline-const-pointer-repair-source-preparation-20261028.json';dest.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n');print(json.dumps({'checks':len(checks),'patch_sha256':sha(patch.read_bytes()),'report_sha256':sha(dest.read_bytes()),'after':report['after']}))
if __name__=='__main__':main()
