"""Independent rejection checks over metadata factories; never save evidence rows."""
import ast,copy,hashlib,json,types
from pathlib import Path
R=Path(__file__).resolve().parents[1]
p=Path(__file__).with_name('historical_rerun_vector_contracts_20261028.py')
tree=ast.parse(p.read_bytes());nodes=[]
for node in tree.body:
    if isinstance(node,ast.For):break
    nodes.append(node)
m=types.ModuleType('historical_metadata_factory');m.__file__=str(p)
exec(compile(ast.Module(body=nodes,type_ignores=[]),str(p),'exec'),m.__dict__)
count=0
def check(value):
    global count
    if not value:raise AssertionError('independent historical metadata rejection failed')
    count+=1
def reject(call):
    try:call()
    except m.checker.VectorError:check(True)
    else:check(False)
for item in m.MANIFEST['items']:
    rows,artifacts,sp,rp,gp,bp,refresh,read=m.scenario(item)
    suite={'fullsuite_build_path':bp} if item['bug']=='C41' else {}
    def audit(values,reader=read):
        return m.checker.audit(m.MANIFEST,item['bug'],sp,refresh(values),reader,**suite)
    result=audit(rows)
    check(result['exact_vector_passed'] and result['automatic_full_finding_promotion'] is False)
    check(result['remaining_qualifications']==item['remaining_qualifications'])
    for index,row in enumerate(rows):
        changed=copy.deepcopy(rows);changed.pop(index)
        reject(lambda:audit(changed))
        changed=copy.deepcopy(rows);changed[index]['case']='unselected_metadata_case'
        reject(lambda:audit(changed))
        cell=next(c for c in item['cells'] if c['case']==row['case'])
        if cell['record_kind']=='generator_snapshot':
            for field in cell['required_oracle_fields']:
                changed=copy.deepcopy(rows);changed[index].pop(field)
                reject(lambda:audit(changed))
            changed=copy.deepcopy(rows);changed[index]['generator_hash_source']='unavailable_direct_import'
            reject(lambda:audit(changed))
        else:
            for field,value in [('repetition',None),('generator_sha256','a'*64),
                                ('optimizer_mode_config_verified',True),('settings_sha256','a'*64)]:
                changed=copy.deepcopy(rows);changed[index][field]=value
                reject(lambda:audit(changed))
    original=artifacts[rp]
    for field in ('size_bytes','sha256','ctime_ns','mtime_ns','inode','device','resolved_path'):
        receipt=m.checker.document(original)
        receipt['binary'].pop(field)
        changed=copy.deepcopy(rows)
        for row in changed:row['binary_provenance'].pop(field)
        artifacts[rp]=m.encode(receipt)
        for row in changed:row['build_receipt_sha256']=m.checker.sha(artifacts[rp])
        reject(lambda:audit(changed))
    artifacts[rp]=original
    for field,value in [('recorded_unix_ns',m.MANIFEST['prepared_unix_ns']-1),
                        ('recorded_unix_ns',m.MANIFEST['prepared_unix_ns']+2000),
                        ('worktree','/unreviewed-worktree')]:
        receipt=m.checker.document(original);receipt[field]=value
        artifacts[rp]=m.encode(receipt)
        changed=copy.deepcopy(rows)
        for row in changed:row['build_receipt_sha256']=m.checker.sha(artifacts[rp])
        reject(lambda:audit(changed))
    artifacts[rp]=original
    # Only the documented ctime revalidation difference is allowed.
    changed=copy.deepcopy(rows)
    for row in changed:
        row['binary_provenance']['ctime_ns']+=1
        row['build_receipt_ctime_changed']=True
    check(audit(changed)['exact_vector_passed'])
    for row in changed:row['build_receipt_ctime_changed']=False
    reject(lambda:audit(changed))
    changed=copy.deepcopy(rows);changed[-1]['native_batch_memory_sha256']='a'*64
    reject(lambda:audit(changed))
    changed=copy.deepcopy(rows);changed[-1]['build_receipt_sha256']='a'*64
    reject(lambda:audit(changed))
    def changed_slice(file):
        value=read(file)
        return value+b'\n' if file==sp else value
    reject(lambda:audit(rows,changed_slice))
    if item['bug']=='C18':
        changed=copy.deepcopy(rows)
        sources=[r for r in changed if 'metamorphic_pair' in r]
        targets=[r for r in changed if 'metamorphic_pair' not in r][:len(sources)]
        for source,target in zip(sources,targets):
            for key in ('metamorphic_pair','metamorphic_role','metamorphic_projection_sha256'):
                target[key]=source.pop(key)
        reject(lambda:audit(changed))
    if item['bug']=='C41':
        for field,value in [('explicit_build_workers',64),('watchdog_stopped',True),
                            ('watchdog_min_host_available_gib',0),('watchdog_max_scope_usage_gib',64)]:
            original_build=artifacts[bp];build=m.checker.document(original_build)
            build['memory'][field]=value;artifacts[bp]=m.encode(build)
            reject(lambda:audit(rows));artifacts[bp]=original_build
report={'status':'independent_metadata_rejections_passed_native_reruns_pending',
        'assertions':count,'manifest_sha256':m.checker.MANIFEST_SHA256,
        'checker_sha256':m.checker.sha(Path(m.checker.__file__).read_bytes()),
        'native_or_build_execution':False,'live_edits':False,
        'synthetic_evidence_persisted':False,'rendered_workloads_persisted':False}
out=R/'verification/historical-rerun-vector-independent-contracts-20261028.json'
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'assertions':count,'report_sha256':hashlib.sha256(out.read_bytes()).hexdigest()}))
