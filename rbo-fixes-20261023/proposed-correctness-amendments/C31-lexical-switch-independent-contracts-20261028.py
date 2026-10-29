"""Source and bounded graph-object checks; no rendered graph/workload is saved."""
import ast,hashlib,json,sys,types
from pathlib import Path
from unittest.mock import patch
R=Path(__file__).resolve().parents[1];D=R/'proposed-correctness-amendments'
sys.path[:0]=[str(R),str(R.parent/'rbo-review-20261021')]
checks=[]
def check(value,label):
    if not value:raise AssertionError(label)
    checks.append(label)
def load(path,name,source=None):
    m=types.ModuleType(name);m.__file__=str(path);sys.modules[name]=m
    exec(compile(path.read_bytes() if source is None else source,str(path),'exec'),m.__dict__)
    return m
helper=D/'c31_shared_field_probe_tuple_guard_20261028.candidate.py'
fields=load(helper,'c31_shared_field_probe')
probe=load(D/'c31_switch_shape_observation_20261028.candidate.py','review_switch')
N=fields._Node;A=fields._Arg
F=('exact_shared_producer_origin_proved','aggregation_value_to_identity_binding_proved',
   'full_shared_producer_contract_pass','candidate_complete_key_pair_lineage_observed')
secret='unlisted_private_marker'
def lam(args,*bodies):return N('lambda',(tuple(args),*bodies))
def graph_observe(nodes):
    graph=types.SimpleNamespace(nodes=set(nodes),block_count=0)
    with patch.object(fields,'_LexicalGraph',return_value=graph):
        result=probe.observe(sorted(fields.CASES)[0],'metadata-only placeholder')
    check(result['physical_ast_sha256']==hashlib.sha256(b'metadata-only placeholder').hexdigest(),'exact_input_hash')
    check(result['observation_only'] is True,'observation_only')
    check(result['raw_graph_material_persisted'] is False,'no_raw_material_flag')
    for flag in F:check(result[flag] is False,'no_proof_'+flag)
    check(secret not in json.dumps(result),'no_unlisted_data_in_result')
    return result
stage=N('DqPhyStage',((),lam([],N('AsList',()))))
for form in ('same','foreign','member_same','member_foreign','unknown','too_deep','mapping_truncated'):
    arg,row,foreign=A(),A(),A()
    if form=='same':body=arg
    elif form=='foreign':body=foreign
    elif form.startswith('member_'):
        member=N('Member',(row if form=='member_same' else foreign,"'"+secret))
        body=N('Map',(arg,lam([row],N('AsStruct',(("'"+secret,member),)))))
    elif form=='unknown':body=N(secret,(arg,))
    elif form=='too_deep':
        body=arg
        for _ in range(13):body=N('ToFlow',(body,))
    else:body=N('WideMap',(arg,lam([row],*[row for _ in range(129)])))
    switch=N('Switch',(A(),"'buffer",("'0",),lam([arg],body),("'0",),lam([A()],A())))
    result=graph_observe([stage,switch])
    check(result['switch_shape_inventory_complete'],'bounded_inventory_completed_'+form)
    count=result['counters']
    if form=='same':check(count['branch_status'].get('direct_same_argument')==1,'same_argument_uses_identity')
    if form=='foreign':check(count['input_chain_terminal'].get('different_argument')==2,'foreign_argument_not_same')
    if form=='member_same':check(count['struct_values']=={'own_row_member':1},'own_member_identity')
    if form=='member_foreign':check(count['struct_values']=={'foreign_row_member':1},'foreign_member_identity')
    if form=='unknown':check(count['branch_root_heads'].get('other_callable')==1,'unknown_head_sanitized')
    if form=='too_deep':check(count['input_chain_terminal'].get('depth_limit')==1,'chain_depth_bounded')
    if form=='mapping_truncated':
        check(count['mapping_body_counts'].get('over_128')==1,'large_mapping_size_bucket')
        check(count['mapping_status'].get('truncated')==1 and count['mapping_status'].get('own_argument')==128,'mapping_work_is_bounded')
for callable_name,indices in probe._JOIN.items():
    children=[N(secret,()) for _ in range(max(indices)+1)]
    children[indices[0]]="'"+secret
    children[indices[1]]=("'"+secret,)
    children[indices[2]]=("'0","'1")
    result=graph_observe([stage,N(callable_name,tuple(children))])
    check(result['counters']['join_kinds']=={'other_kind':1},'join_kind_sanitized_'+callable_name)
    check(result['counters']['join_status'].get('different_key_counts')==1,'join_width_metadata_'+callable_name)
for kind,count in [('DqPhyStage',129),('Switch',33),('GraceJoinCore',33)]:
    nodes=[N(kind,()) for _ in range(count)]
    if kind!='DqPhyStage':nodes.append(stage)
    result=graph_observe(nodes)
    check(result['switch_shape_inventory_complete'] is False and result['switch_shape_inventory_rejection']=='inventory_budget','inventory_budget_'+kind)

def named(t,kind,name):return next(x for x in t.body if isinstance(x,kind) and x.name==name)
before=ast.parse((R/'c31_shared_field_probe.py').read_bytes())
after=ast.parse(helper.read_bytes())
dump=lambda x:ast.dump(x,include_attributes=False)
check(dump(named(before,ast.ClassDef,'_Proof'))==dump(named(after,ast.ClassDef,'_Proof')),'entire_existing_proof_class_unchanged')
for node in before.body:
    if isinstance(node,ast.FunctionDef):
        check(dump(node)==dump(named(after,ast.FunctionDef,node.name)),'helper_function_unchanged_'+node.name)
old_wrapper=ast.parse((R/'c31_scope_observation.py').read_bytes())
new_wrapper=ast.parse((D/'c31_scope_observation_switch_shapes_20261028.candidate.py').read_bytes())
for name in ('metadata','recipes','_inventory'):
    check(dump(named(old_wrapper,ast.FunctionDef,name))==dump(named(new_wrapper,ast.FunctionDef,name)),'wrapper_'+name+'_unchanged')
# Every _Reject raised by the inherited helper has a fixed literal label.
for node in ast.walk(after):
    if isinstance(node,ast.Raise) and isinstance(node.exc,ast.Call) and isinstance(node.exc.func,ast.Name) and node.exc.func.id=='_Reject':
        check(len(node.exc.args)==1 and isinstance(node.exc.args[0],ast.Constant) and isinstance(node.exc.args[0].value,str),'fixed_helper_rejection_label')
# Independently verify identity-deduplication cost on directly parsed metadata.
counted=helper.read_text().replace('        while pending:\n','        self.review_visits = 0\n        while pending:\n            self.review_visits += 1\n',1)
instrumented=load(helper,'review_counted_lexical',counted)
samples=[]
for depth in (4,12,48,160):
    statements=[['let','$review0',["'opaque"]]]
    for i in range(1,depth+1):
        statements.append(['let','$review'+str(i),['$review'+str(i-1),'$review'+str(i-1)]])
    statements.append(['return',['DqPhyStage',[],['lambda',[],'$review'+str(depth)]]])
    with patch.object(instrumented.windows,'_parse_ast',return_value=statements):
        graph=instrumented._LexicalGraph('metadata only')
    check(len(graph.nodes)==2,'tuple_dedup_preserves_reachable_nodes')
    check(graph.review_visits<=3*depth+10,'tuple_walk_linear_bound')
    samples.append(dict(normalization_visits=graph.count,reachability_visits=graph.review_visits))
report={'status':'independent_source_and_graph_object_metadata_passed_actual_observation_pending',
        'assertions':len(checks),'checks':checks,'tuple_walk_samples':samples,
        'native_or_build_execution':False,'live_changes':False,'rendered_graphs_persisted':False,
        'full_lineage_proof_promoted':False}
out=R/'verification/C31-lexical-switch-independent-contracts-20261028.json'
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'assertions':len(checks),'report_sha256':hashlib.sha256(out.read_bytes()).hexdigest()}))
