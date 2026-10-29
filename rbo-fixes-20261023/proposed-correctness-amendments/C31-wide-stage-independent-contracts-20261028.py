"""Independent bounded observation/composition checks; no payloads persisted."""
import ast,hashlib,json,subprocess,sys,tempfile,types
from pathlib import Path
from unittest.mock import patch
R=Path(__file__).resolve().parents[1];D=Path(__file__).resolve().parent
sys.path[:0]=[str(R),str(R.parent/'rbo-review-20261021')]
checks=[]
def check(value,label):
    if not value:raise AssertionError(label)
    checks.append(label)
def load(path,name,source=None):
    m=types.ModuleType(name);m.__file__=str(path);sys.modules[name]=m
    exec(compile(path.read_bytes() if source is None else source,str(path),'exec'),m.__dict__)
    return m
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest=json.loads((R/'verification/C31-wide-observer-and-stage-tuple-composition-20261028.json').read_bytes())
with tempfile.TemporaryDirectory(prefix='c31-wide-independent-') as tmp:
    target=Path(tmp)/'rbo-fixes-20261023';target.mkdir()
    for name,digest in manifest['live_before_sha256'].items():
        check(sha(R/name)==digest,'live_before_'+name)
        (target/name).write_bytes((R/name).read_bytes())
    for spec in manifest['ordered_patches']:
        check(sha(R/spec['path'])==spec['sha256'],'frozen_patch_digest')
        done=subprocess.run(['git','apply','--unsafe-paths','--whitespace=error-all',str(R/spec['path'])],
                            cwd=tmp,capture_output=True)
        check(done.returncode==0,'clean_temporary_application')
    for name,digest in manifest['combined_after_sha256'].items():check(sha(target/name)==digest,'composed_after_'+name)
    wrapper=load(target/'c31_scope_observation.py','review_c31_wrapper')
    observer=wrapper._switches;fields=wrapper._fields
    check(observer.fields is fields,'shared_patched_helper_snapshot')
    check(wrapper._HASHES['c31_shared_field_probe']==sha(target/'c31_shared_field_probe.py'),'patched_helper_source_binding')
    expected_cases=('c31_stability_float_domain__spilling__left','c31_stability_integer_domain__spilling__left')
    check(wrapper.CASES==expected_cases,'exact_two_profile_scope')
    for case in expected_cases:
        meta=wrapper.metadata(case,24)
        check(meta['table_service_overrides']['EnableSpillingNodes']=='None','node_spill_mask_unchanged')
        check(meta['table_service_overrides']['SpillingServiceConfig']=='{ LocalFileConfig { Enable: true MaxTotalSize: 1 } }','one_byte_spill_cap_unchanged')
        for rows in (True,23,25):
            try:wrapper.metadata(case,rows)
            except ValueError:check(True,'unreviewed_rows_rejected')
            else:check(False,'unreviewed_rows_rejected')
    N,A=fields._Node,fields._Arg
    lam=lambda args,*bodies:N('lambda',(tuple(args),*bodies))
    flags=('exact_shared_producer_origin_proved','aggregation_value_to_identity_binding_proved',
           'full_shared_producer_contract_pass','candidate_complete_key_pair_lineage_observed')
    secret='private_unlisted_marker'
    variants=['plain','permuted','same_decoded_names','foreign_scalar','duplicate_scalar',
              'omitted_scalar','computed','foreign_input','unknown_wrapper','bad_wrapper_arity','two_bodies']
    for width in (1,2,128,129):
        for variant in variants:
            if width==1 and variant in ('same_decoded_names','duplicate_scalar'):continue
            branch,foreign=A(),A();rows=[A() for _ in range(width)]
            pairs=[("'"+secret+str(i),row) for i,row in enumerate(rows)]
            if variant=='permuted':pairs.reverse()
            elif variant=='same_decoded_names':
                pairs[0]=("'\""+secret+"\"",pairs[0][1]);pairs[-1]=("'"+secret,pairs[-1][1])
            elif variant=='foreign_scalar':pairs[-1]=(pairs[-1][0],foreign)
            elif variant=='duplicate_scalar':pairs[-1]=(pairs[-1][0],rows[0])
            elif variant=='omitted_scalar':pairs.pop()
            elif variant=='computed':pairs[-1]=(pairs[-1][0],N('StablePickle',(rows[-1],)))
            body=N('AsStruct',tuple(pairs))
            codec=N(secret if variant=='unknown_wrapper' else 'WideFromBlocks',
                    ((foreign if variant=='foreign_input' else branch),foreign) if variant=='bad_wrapper_arity'
                    else ((foreign if variant=='foreign_input' else branch),))
            mapping=N('NarrowMap',(codec,lam(rows,body,*([body] if variant=='two_bodies' else []))))
            switch=N('Switch',(A(),"'buffer",("'0",),lam([branch],mapping),("'0",),lam([A()],A())))
            stage=N('DqPhyStage',((),lam([],switch)))
            graph=types.SimpleNamespace(nodes={stage,switch},block_count=0)
            with patch.object(fields,'_LexicalGraph',return_value=graph):
                result=observer.observe(expected_cases[0],'metadata only')
            check(result['observation_only'] and result['switch_shape_inventory_complete'],'observation_only_complete')
            for flag in flags:check(result[flag] is False,'no_proof_promotion_'+flag)
            check(secret not in json.dumps(result),'unlisted_values_not_emitted')
            local_good=width<=128 and variant in ('plain','permuted','foreign_input','unknown_wrapper','bad_wrapper_arity')
            counts=result['counters']['narrow_projection_shape']
            check(counts=={('own_argument_bijection' if local_good else 'not_own_argument_bijection'):
                           (2 if variant=='two_bodies' else 1)},'local_bijection_is_bounded_and_exact')
            if variant=='foreign_input':check(result['counters']['input_chain_terminal'].get('different_argument')==2,'foreign_input_remains_separate')
            if variant=='unknown_wrapper':check(result['counters']['input_chain_terminal'].get('unsupported_shape')==1,'unknown_wrapper_rejected')
            if variant=='bad_wrapper_arity':check(result['counters']['input_chain_terminal'].get('unsupported_arity')==1,'bad_wrapper_arity_rejected')
    # Validate the proof change is precisely the identity-set optimization.
    before=(R/'c31_shared_field_probe.py').read_text();after=(target/'c31_shared_field_probe.py').read_text()
    expected=before.replace('            pending, seen, local_joins = [stage.children[1]], set(), set()\n',
        '            pending, seen, local_joins = [stage.children[1]], set(), set()\n            seen_tuples = set()\n',1)
    expected=expected.replace('                elif isinstance(value, tuple):\n                    pending.extend(value)\n',
        '                elif isinstance(value, tuple):\n                    identity = id(value)\n                    if identity in seen_tuples:\n                        continue\n                    seen_tuples.add(identity)\n                    pending.extend(value)\n',1)
    check(after==expected,'only_stage_local_tuple_identity_guard_changed')
    # Both stages intentionally share one program. Per-stage tuple visitation
    # must still find joins in both stages and preserve the existing rejection.
    for depth in (0,1,8,128,512):
        join1,join2=N('GraceJoinCore',()),N('GraceJoinCore',())
        shared=(join1,join2)
        for _ in range(depth):shared=(shared,shared)
        program=lam([],shared)
        stages=[N('DqPhyStage',((),program)),N('DqPhyStage',((),program))]
        graph=types.SimpleNamespace(nodes={join1,join2,*stages})
        proof=fields._Proof(graph);seen=[]
        proof._stage=lambda stage:seen.append(stage)
        try:proof.verify(expected_cases[0])
        except fields._Reject as e:check(str(e)=='untraced_join','existing_proof_rejection_preserved')
        else:check(False,'existing_proof_rejection_preserved')
        check(set(seen)==set(stages) and len(seen)==2,'shared_program_not_skipped_in_second_stage')
    # Wrapper's recipe/oracle plumbing stays unchanged apart from version.
    tree0=ast.parse((R/'c31_scope_observation.py').read_bytes())
    tree1=ast.parse((target/'c31_scope_observation.py').read_bytes())
    funcs=lambda t:{n.name:ast.dump(n,include_attributes=False) for n in t.body if isinstance(n,ast.FunctionDef)}
    check(funcs(tree0)==funcs(tree1),'wrapper_functions_unchanged')
    check(wrapper.schema_and_seeds is wrapper._base.schema_and_seeds and wrapper.check_rows is wrapper._base.check_rows,'original_fixture_and_row_oracle_delegation')
for name,digest in manifest['live_before_sha256'].items():check(sha(R/name)==digest,'live_stays_unchanged_'+name)
report={'status':'independent_source_and_graph_object_metadata_passed_native_observations_pending',
        'assertions':len(checks),'checks':checks,'native_or_build_execution':False,'live_edits':False,
        'rendered_graphs_queries_or_cpp_persisted':False,'lineage_admission_promoted':False}
out=R/'verification/C31-wide-stage-independent-contracts-20261028.json'
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'assertions':len(checks),'report_sha256':sha(out)}))
