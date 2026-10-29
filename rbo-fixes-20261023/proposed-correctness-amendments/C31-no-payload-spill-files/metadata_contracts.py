from pathlib import Path
import ast, copy, hashlib, importlib.util, json, re, sys, tempfile, subprocess
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory, text_format
ROOT=Path.cwd(); A=ROOT/'rbo-fixes-20261023'; D=A/'proposed-correctness-amendments/C31-no-payload-spill-files'
checks=[]
def check(label, value):
    if not value: raise AssertionError(label)
    checks.append(label)
def load_source(name, source):
    module=type(sys)(name); module.__file__=str(A/'c31_scope_observation.py');sys.modules[name]=module
    exec(compile(source,module.__file__,'exec'),module.__dict__); return module
before=(A/'c31_scope_observation.py').read_text(); after=(D/'c31_scope_observation.py').read_text()
before_hash=hashlib.sha256(before.encode()).hexdigest()
sys.path.insert(0,str(ROOT/'rbo-review-20261021'))
sys.path.insert(0,str(A))
old=load_source('c31_guard_before_metadata',before);new=load_source('c31_guard_after_metadata',after)
check('selected_profiles_unchanged',old.CASES==new.CASES and len(new.CASES)==2)
for fn in ('recipes','_inventory','check_physical_ast'):
    nodes=[]
    for src in (before,after):
        nodes.append(next(n for n in ast.parse(src).body if isinstance(n,ast.FunctionDef) and n.name==fn))
    check(fn+'_source_unchanged',ast.dump(nodes[0],include_attributes=False)==ast.dump(nodes[1],include_attributes=False))
check('schema_delegate_identity',new.schema_and_seeds is new._base.schema_and_seeds)
check('row_oracle_delegate_identity',new.check_rows is new._base.check_rows)
check('dependency_snapshots_unchanged',new._HASHES==old._HASHES)
for case in new.CASES:
    original=old.metadata(case,24);candidate=new.metadata(case,24);restored=copy.deepcopy(candidate)
    restored['generator_version']=original['generator_version']
    restored['table_service_overrides']['EnableSpillingNodes']=original['table_service_overrides']['EnableSpillingNodes']
    restored['table_service_overrides'].pop('SpillingServiceConfig')
    check(case+'_only_reviewed_metadata_changed',restored==original)
    ov=candidate['table_service_overrides']
    check(case+'_channel_flag_preserved',ov['EnableQueryServiceSpilling'] is True)
    check(case+'_node_mask_disabled',ov['EnableSpillingNodes']=='None')
    check(case+'_service_cap_exact',ov['SpillingServiceConfig']=='{ LocalFileConfig { Enable: true MaxTotalSize: 1 } }')
    check(case+'_one_node',candidate['node_count']==1)
    check(case+'_no_rejection_admission','expected_rejection_token' not in candidate)
    check(case+'_physical_evidence_required',candidate['correctness_requires_physical_ast'] is True)
    check(case+'_sharing_expected',candidate['expected_shared_producer'] is True)
    check(case+'_row_hash_join_preserved',ov['UseBlockHashJoin'] is False)
    check(case+'_base_metadata_not_mutated','SpillingServiceConfig' not in new._base.metadata(case,24)['table_service_overrides'])
    for n in (0,1,23,25,False,24.0,'24',None):
        try:new.metadata(case,n)
        except ValueError:checks.append(case+'_reject_rows_'+repr(n))
        else:raise AssertionError('unsupported row count accepted')
try:new.metadata('unselected',24)
except ValueError:checks.append('unknown_profile_rejected')
else:raise AssertionError('unknown profile accepted')
# Run only the existing config renderer, extracted from its source AST.
helper_source=(A/'native_verify.py').read_text();tree=ast.parse(helper_source)
node=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='config_with')
ns={'re':re,'json':json};exec(compile(ast.Module(body=[node],type_ignores=[]),'metadata-only-config-renderer','exec'),ns)
base=(ROOT/'rbo-review-20261021/new.conf').read_text()
check('base_has_no_existing_spill_service_message','SpillingServiceConfig' not in base)
# A schema-shaped subset validates text-format nesting and scalar types while
# accepting unrelated config fields. This is not a native full-config parse.
f=descriptor_pb2.FileDescriptorProto(name='c31_metadata_only.proto',syntax='proto2',package='C31Metadata')
def msg(name):return f.message_type.add(name=name)
def fld(m,name,num,kind,type_name=None):
    x=m.field.add(name=name,number=num,label=1,type=kind)
    if type_name:x.type_name='.C31Metadata.'+type_name
local=msg('Local');fld(local,'Enable',1,8);fld(local,'MaxTotalSize',3,4)
service=msg('Service');fld(service,'LocalFileConfig',1,11,'Local')
table=msg('Table');fld(table,'SpillingServiceConfig',16,11,'Service');fld(table,'EnableQueryServiceSpilling',63,8);fld(table,'EnableSpillingNodes',67,9);fld(table,'UseBlockHashJoin',124,8)
app=msg('App');fld(app,'TableServiceConfig',1,11,'Table')
pool=descriptor_pool.DescriptorPool();pool.Add(f);cls=message_factory.MessageFactory(pool).GetPrototype(pool.FindMessageTypeByName('C31Metadata.App'))
config_hashes={}
for case in new.CASES:
    config=ns['config_with'](base,new.metadata(case,24)['table_service_overrides'])
    parsed=cls();text_format.Parse(config,parsed,allow_unknown_field=True)
    t=parsed.TableServiceConfig;s=t.SpillingServiceConfig.LocalFileConfig
    check(case+'_protobuf_subset_message_present',t.HasField('SpillingServiceConfig') and t.SpillingServiceConfig.HasField('LocalFileConfig'))
    check(case+'_protobuf_subset_cap',s.MaxTotalSize==1 and s.Enable)
    check(case+'_protobuf_subset_channel_enabled',t.EnableQueryServiceSpilling)
    check(case+'_protobuf_subset_mask',t.EnableSpillingNodes=='None')
    check(case+'_protobuf_subset_join',not t.UseBlockHashJoin)
    check(case+'_one_service_message',config.count('SpillingServiceConfig:')==1)
    config_hashes[case]=hashlib.sha256(config.encode()).hexdigest()
# Apply the exact dormant patch to a temporary copy; no live mutation.
patch=A/'proposed-correctness-amendments/C31-observation-no-payload-spill-files-20261028.patch'
with tempfile.TemporaryDirectory(prefix='c31-spill-metadata-patch-') as temp:
    d=Path(temp)/'rbo-fixes-20261023';d.mkdir();(d/'c31_scope_observation.py').write_text(before)
    done=subprocess.run(['git','apply','--unsafe-paths','--whitespace=error-all',str(patch)],cwd=temp,capture_output=True)
    check('dormant_patch_applies',done.returncode==0)
    check('dormant_patch_exact_after_image',(d/'c31_scope_observation.py').read_text()==after)
check('live_module_unchanged',hashlib.sha256((A/'c31_scope_observation.py').read_bytes()).hexdigest()==before_hash)
result={'scope':'metadata/config-subset-parser/patch-application only','assertions_passed':len(checks),'checks':checks,'source_binding_sha256':new._HASHES,'rendered_config_sha256':config_hashes,'rendered_workloads_created':False,'native_processes_started':False,'full_native_config_parse':'pending','live_module_unchanged':True}
(D/'metadata-check-results.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'assertions_passed':len(checks),'live_module_unchanged':True,'native_processes_started':False}))
