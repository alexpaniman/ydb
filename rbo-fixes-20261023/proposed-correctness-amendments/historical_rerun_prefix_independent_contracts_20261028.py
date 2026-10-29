"""Recompute frozen modern metadata from the actual preparation prefix in RAM."""
import ast,hashlib,importlib.util,json,sys,types
from pathlib import Path
R=Path('/home/alexpaniman/projects/ydb/ai-verification/rbo-fixes-20261023')
sys.path[:0]=[str(R),str(R.parent/'rbo-review-20261021')]
manifest=json.loads((R/'verification/historical-rerun-exact-vector-20261028.json').read_bytes())
import native_verify as native
tree=ast.parse((R/'native_verify.py').read_bytes())
f=next(x for x in tree.body if isinstance(x,ast.FunctionDef) and x.name=='run_generated')
prefix=[]
for node in f.body:
    if isinstance(node,ast.Try):break
    if isinstance(node,ast.FunctionDef) and node.name=='memory':
        node.body=ast.parse("descriptors.append(0)\nreturn 'anonymous-placeholder'").body
    prefix.append(node)
f.body=prefix+ast.parse('return record').body
f.name='review_actual_prefix'
ast.fix_missing_locations(f)
space=dict(native.__dict__)
exec(compile(ast.Module(body=[f],type_ignores=[]),'metadata-prefix-only','exec'),space)
checks=0
def check(value):
    global checks
    if not value:raise AssertionError('independent prefix metadata mismatch')
    checks+=1
modules={name:native.import_generator_snapshot(name) for name in manifest['default_modules']}
available={}
collisions=[]
for name,m in modules.items():
    for case in m.recipes(24):
        if case in available:collisions.append(case)
        available[case]=m
selected=[c for i in manifest['items'] for c in i['cells'] if c['record_kind']=='generator_snapshot']
check(not set(collisions)&{c['case'] for c in selected})
for cell in selected:
    m=available[cell['case']]
    check(m.__name__==cell['generator_module'])
    meta=native.module_metadata(m,cell['case'],cell['mode'],24)
    check(cell['repetitions']==list(range(meta.get('repetitions',1))))
    actual=space['review_actual_prefix'](m,cell['case'],cell['mode'],24)
    for key in ('generator_module','generator_sha256','generator_version','node_count','query_syntax'):
        check(type(actual[key]) is type(cell[key]) and actual[key]==cell[key])
    for key,value in cell['bindings'].items():check(type(actual[key]) is type(value) and actual[key]==value)
    for key,value in cell['required_record_fields'].items():
        # Post-execution oracle requirements are independently reviewed from
        # their actual generator methods; this prefix has no result rows.
        if key in actual:check(type(actual[key]) is type(value) and actual[key]==value)
    expected=('guard' if meta.get('expected_rejection_token') else
              'target_failure' if getattr(m,'EXPECTED_FAILURES',{}).get(cell['case'])=='scalar cardinality'
              else 'functional')
    check(cell['expected_outcome']==expected)
check(len(selected)==87)
check(sum(len(c['repetitions']) for c in selected)==101)
check(sum(c['record_kind']=='legacy_correlation_adapter' for i in manifest['items'] for c in i['cells'])==8)
print(json.dumps({'metadata_prefix_checks':checks,'modern_case_count':len(selected),
                  'modern_record_count':101,'legacy_qualified_record_count':8,
                  'native_calls':0,'rendered_payloads_persisted':False}))
