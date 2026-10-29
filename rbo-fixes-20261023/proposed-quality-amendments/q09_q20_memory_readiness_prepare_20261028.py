"""Prepare a dormant scoped adapter change; no native/build actions."""
import ast
import copy
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT.parent
OUT = ROOT / 'proposed-quality-amendments/Q09-Q20-memory-readiness-20261028'
sys.dont_write_bytecode = True


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def literals(raw):
    result = {}
    for node in ast.parse(raw).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            try:
                result[node.targets[0].id] = ast.literal_eval(node.value)
            except (ValueError, TypeError):
                pass
    return result


def functions(raw):
    return {node.name: ast.dump(node) for node in ast.parse(raw).body if isinstance(node, (ast.FunctionDef, ast.ClassDef))}


def main():
    checks = []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    pins = {
        'anonymous_cpp_verify.py':'836054297fe6954e2bbab441f60e9cfd7d0673394a42f2b1aa4de9eb5e392136',
        'quality_spill_bounds.py':'6bd8b204c5846a274109f152998a08d73497c4b232c0a2cf4730ae746b51daf6',
        'c06_c50_bounded_integration_checks.py':'f8a5b3919d3f3eec715f29efebbc69ca8e0f593418eb16458643190f0252dd3e',
        'c06_c50_integration_runner.py':'af935fa5acaf2da230304b6ba7da9bf2e484f44ffacb79af8e5c6f585fc7ae03',
        'q28_depth_verify.py':'a5cdd984048f249ea8c616f17b799b4ee0eb09bc44cd3fe00c4e8314db02ce6b',
    }
    original = {name:(ROOT / name).read_bytes() for name in pins}
    for name, expected in pins.items():
        check(sha(original[name]) == expected, 'exact_idle60_before:' + name)
    images = dict(original)
    raw = images['anonymous_cpp_verify.py']
    substitutions = [
        (b"export_objects=args.bug not in ('C31', 'C47')", b"export_objects=args.bug not in ('C31', 'C47', 'Q09', 'Q20')"),
        (b"'broad_object_export': args.bug not in ('C31', 'C47')", b"'broad_object_export': args.bug not in ('C31', 'C47', 'Q09', 'Q20')"),
        (b"if args.bug in ('C31', 'C47'):\n            # Drop dependency DWARF", b"if args.bug in ('C31', 'C47', 'Q09', 'Q20'):\n            # Drop dependency DWARF"),
    ]
    for before, after in substitutions:
        check(raw.count(before) == 1, 'single_scoped_condition:' + before.decode().split(':')[0])
        raw = raw.replace(before, after)
    images['anonymous_cpp_verify.py'] = raw
    anonymous_hash = sha(raw)
    def replace_hash(name, before, after):
        check(images[name].count(before.encode()) == 1, 'single_binding_update:' + name + ':' + before[:8])
        images[name] = images[name].replace(before.encode(), after.encode())
    replace_hash('quality_spill_bounds.py', pins['anonymous_cpp_verify.py'], anonymous_hash)
    replace_hash('c06_c50_bounded_integration_checks.py', pins['quality_spill_bounds.py'], sha(images['quality_spill_bounds.py']))
    replace_hash('c06_c50_integration_runner.py', pins['anonymous_cpp_verify.py'], anonymous_hash)
    replace_hash('c06_c50_integration_runner.py', pins['quality_spill_bounds.py'], sha(images['quality_spill_bounds.py']))
    replace_hash('c06_c50_integration_runner.py', pins['c06_c50_bounded_integration_checks.py'], sha(images['c06_c50_bounded_integration_checks.py']))
    replace_hash('q28_depth_verify.py', pins['anonymous_cpp_verify.py'], anonymous_hash)
    # Normalize just the three reviewed tuple operands, then compare complete ASTs.
    parsed_old, parsed_new = ast.parse(original['anonymous_cpp_verify.py']), ast.parse(images['anonymous_cpp_verify.py'])
    changed = 0
    for function in parsed_new.body:
        if not isinstance(function, ast.FunctionDef) or function.name not in ('prepare','execute'):
            continue
        for node in ast.walk(function):
            if isinstance(node, ast.Compare) and len(node.comparators) == 1:
                operand = node.comparators[0]
                if isinstance(operand, ast.Tuple) and [n.value for n in operand.elts if isinstance(n, ast.Constant)] == ['C31','C47','Q09','Q20']:
                    operand.elts = operand.elts[:2]
                    changed += 1
    check(changed == 3 and ast.dump(parsed_new) == ast.dump(parsed_old), 'only_three_scoped_tuple_changes_in_entire_adapter')
    for name in pins:
        if name == 'anonymous_cpp_verify.py':
            continue
        check(functions(original[name]) == functions(images[name]), 'unchanged_all_functions:' + name)
        old, new = ast.parse(original[name]), ast.parse(images[name])
        def scrub(tree):
            for node in tree.body:
                if isinstance(node, ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('SOURCE_BINDINGS','_SOURCE_BINDINGS') for t in node.targets):
                    node.value = ast.Constant(None)
            return ast.dump(tree)
        check(scrub(old) == scrub(new), 'only_existing_dependency_dictionary_changed:' + name)

    sys.path[:0] = [str(ROOT),str(PROJECT/'rbo-review-20261021')]
    import anonymous_cpp_metadata as md
    import quality_statistics_cpp_contract as statistics
    current = ast.parse(images['anonymous_cpp_verify.py'])
    prepare = next(n for n in current.body if isinstance(n,ast.FunctionDef) and n.name == 'prepare')
    execute = next(n for n in current.body if isinstance(n,ast.FunctionDef) and n.name == 'execute')
    export_call = next(n for n in ast.walk(prepare) if isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id=='production_build_arguments')
    export_expr = next(k.value for k in export_call.keywords if k.arg=='export_objects')
    record = next(n.value for n in prepare.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='record' for t in n.targets))
    record_expr = next(v for k,v in zip(record.keys,record.values) if isinstance(k,ast.Constant) and k.value=='broad_object_export')
    strip = next(n for n in ast.walk(execute) if isinstance(n,ast.If) and any(
        isinstance(x,ast.Constant) and x.value=='-Wl,--strip-debug' for x in ast.walk(n)))
    selected = {'C31','C47','Q09','Q20'}
    generators = literals(images['anonymous_cpp_verify.py'])['GENERATORS']
    for bug in generators:
        for phase in ('baseline','fixed'):
            args = SimpleNamespace(bug=bug,phase=phase)
            namespace = {'args':args,'link_argv':['production-compiler','production-archive.a','-o','-'],
                         'evidence':{},'_json_digest':lambda value:sha(json.dumps(value,sort_keys=True,separators=(',',':')).encode())}
            export = eval(compile(ast.Expression(export_expr), '<RAM-export-condition>', 'eval'), namespace)
            recorded = eval(compile(ast.Expression(record_expr), '<RAM-record-condition>', 'eval'), namespace)
            check(export is (bug not in selected) and export is recorded, 'build_and_record_export_agree:' + bug+':'+phase)
            actual_args = md.production_build_arguments('rbo',Path('/metadata-only-output'),Path('/metadata-only-graph'),export_objects=export)
            check(('--add-result=.o' in actual_args) is (bug not in selected), 'actual_object_export_selection:'+bug+':'+phase)
            check(actual_args[0]=='ydb/core/kqp/ut/rbo' and '--all-outputs-to-result' in actual_args,
                  'production_target_and_archive_export_preserved:'+bug+':'+phase)
            exec(compile(ast.Module(body=[strip],type_ignores=[]),'<RAM-strip-branch>','exec'),namespace)
            expected = ['production-compiler','production-archive.a','-o','-'] + (['-Wl,--strip-debug'] if bug in selected else [])
            check(namespace['link_argv']==expected,'exact_anonymous_argv_change:'+bug+':'+phase)
            check(('link_debug_info' in namespace['evidence']) is (bug in selected),'debug_adaptation_evidence_presence:'+bug+':'+phase)
            if bug in selected:
                check(namespace['evidence']['link_debug_info']=={'option':'-Wl,--strip-debug','adapted_argv_template_sha256':namespace['_json_digest'](expected)},
                      'exact_adaptation_hash:'+bug+':'+phase)
    coverage = {}
    profiles = json.loads((ROOT/'quality_statistics_cpp_profiles.json').read_bytes())
    for bug in ('Q09','Q20'):
        module, digest, vector = statistics.current_contract(bug)
        coverage[bug] = {'case_count':len(vector['case_names']),'checks':sum(module.expected_checks(bug)),
                        'generator_sha256':digest,'generated_source_sha256':profiles[bug]['generated_source_sha256']}
        check(coverage[bug]['case_count']==(38 if bug=='Q09' else 72),'original_exact_statistics_vector:'+bug)
        check(digest==profiles[bug]['generator_sha256'],'original_statistics_generator_pin:'+bug)
    for name in pins:
        key = '_SOURCE_BINDINGS' if name=='quality_spill_bounds.py' else 'SOURCE_BINDINGS'
        if name=='anonymous_cpp_verify.py':
            continue
        bindings=literals(images[name])[key]
        check(set(bindings)==set(literals(original[name])[key]),'same_dependency_keys:'+name)
        for dependency, expected in bindings.items():
            path=(ROOT/(dependency+'.py')) if name=='q28_depth_verify.py' else (PROJECT/dependency)
            data=images.get(path.name) if path.parent==ROOT else None
            if data is None:data=path.read_bytes()
            check(sha(data)==expected,'fresh_exact_dependency:'+name+':'+dependency)
    # Evidence is existing, already sanitized metadata, never linker output text.
    evidence_paths=['verification/C31-link-output-elf-sections-20261027.json',
                    'verification/direct-run-C31-1793157890139265085.json',
                    'verification/direct-run-C47-1793148171278692365.json']
    evidence_pins={name:sha((ROOT/name).read_bytes()) for name in evidence_paths}
    elf=json.loads((ROOT/evidence_paths[0]).read_bytes())
    debug_sections=[s for s in elf['sections'] if s['section'].startswith('.debug_')]
    check(sum(s['bytes'] for s in debug_sections)==9185386118 and all(s['alloc'] is False for s in debug_sections),
          'existing_measured_nonallocated_DWARF_inventory')
    observed=[]
    for name in evidence_paths[1:]:
        value=json.loads((ROOT/name).read_bytes())
        check(value['status']=='direct_local_contracts_passed' and value['link']['exit_code']==0
              and value['link']['watchdog_stopped'] is False and value['link']['cleanup_complete'] is True
              and value['link_debug_info']['option']=='-Wl,--strip-debug','existing_actual_stripped_link_success:'+value['bug'])
        receipt_path=Path(value['closure_receipt_path']); raw=receipt_path.read_bytes();receipt=json.loads(raw)
        check(sha(raw)==value['closure_receipt_sha256'],'unchanged_exact_historical_closure_receipt:'+value['bug'])
        check(receipt['metadata']['profile']=='rbo' and receipt['build']['broad_object_export'] is False,
              'existing_same_profile_narrow_export:'+value['bug'])
        check(not any(a.endswith('.o') for a in receipt['metadata']['link_argv']),
              'existing_no_standalone_object_inputs:'+value['bug'])
        observed.append({'bug':value['bug'],'peak_scope_usage_bytes':value['link']['peak_scope_usage_bytes'],
                         'generated_executable_bytes':value['generated_executable']['size_bytes'],
                         'closure_receipt_sha256':value['closure_receipt_sha256']})
    git=lambda *args:subprocess.check_output(['git','-C',str(PROJECT),*args],stderr=subprocess.PIPE)
    production_scope={}
    for bug in ('Q09','Q20'):
        case=json.loads((ROOT/'cases'/(bug+'.json')).read_bytes())
        paths=git('diff','--name-only',case['base'],case['commit']).decode().splitlines()
        check(all(p.startswith('ydb/core/kqp/opt/rbo/') and not p.endswith('ya.make') for p in paths),
              'no_build_graph_or_essentials_change:'+bug)
        production_scope[bug]={'revision':case['commit'],'changed_paths':paths}

    OUT.mkdir(exist_ok=True)
    for name,raw in images.items():(OUT/name).write_bytes(raw)
    diffs=[]
    for name in pins:
        path='rbo-fixes-20261023/'+name
        diffs.append('diff --git a/'+path+' b/'+path+'\n'+''.join(difflib.unified_diff(
            original[name].decode().splitlines(True),images[name].decode().splitlines(True),fromfile='a/'+path,tofile='b/'+path)))
    patch_path=ROOT/'proposed-quality-amendments/Q09-Q20-memory-readiness-20261028.patch'
    patch_path.write_text(''.join(diffs))
    for name,expected in pins.items():check(sha((ROOT/name).read_bytes())==expected,'live_helper_unchanged:'+name)
    report={'schema':'Q09_Q20_memory_readiness_source_preparation_v1','status':'dormant_source_metadata_checked_independent_review_pending',
        'patch':str(patch_path.relative_to(ROOT)),'patch_sha256':sha(patch_path.read_bytes()),
        'before_images':pins,'after_images':{name:sha(raw) for name,raw in images.items()},'checks_passed':len(checks),'checks':checks,
        'source_script_sha256':sha(Path(__file__).read_bytes()),'existing_evidence':evidence_pins,'existing_actual_links':observed,
        'production_scope':production_scope,'unchanged_statistics_coverage':coverage,
        'changes':['Q09 and Q20 join the existing C31/C47 narrow object-export policy for future prepare commands.',
                   'Q09 and Q20 join the existing anonymous-only --strip-debug link policy, with the existing adapted-argv hash evidence.',
                   'Four existing consumers receive only exact dependency hash updates; no new dependency keys or policy behavior.'],
        'preserved':['Active Q09 broad-export build/graph/receipt remains immutable and usable: export_graph/capture read its recorded policy.',
                     'Production archives, generated headers, compiler arguments, ordered linker inputs and production-version replay remain exact.',
                     'Missing standalone inputs still fail metadata capture. No metadata stub or borrowed object is introduced.',
                     'Build workers8 and all build/link/native memory guards, timeouts, cleanup, source/vector/receipt gates remain unchanged.',
                     'Q09 fixed38 and Q20 fixed72 assertions, paired baseline classifications and separate SQL admission remain required.',
                     'Existing Q08/Q02 and correctness evidence keeps its historical helper/profile identities. New batches need fresh profiles.'],
        'limitations':['No Q09/Q20 memory reduction has been measured; existing C31/C47 figures are same-target evidence, not a paired benchmark.',
                       'Narrow object export reduces unnecessary exported files; it is not a proven bound on production build peak memory.',
                       'Anonymous executable source/line DWARF debugging is unavailable under --strip-debug. Normal linking/symbol resolution is retained.',
                       'This changes only verification adapters, not any standalone production fix or optimization quality threshold.'],
        'installation':'Apply the five-file patch at the next idle boundary, never while the active prepare process uses live helpers.',
        'live_edits':False,'native_build_toolchain_executed':False,'generated_queries_CPP_or_rows_persisted':False}
    report_path=ROOT/'verification/Q09-Q20-memory-readiness-source-preparation-20261028.json'
    report_path.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    print(json.dumps({'checks_passed':len(checks),'patch_sha256':report['patch_sha256'],'report_sha256':sha(report_path.read_bytes()),
                      'after_images':report['after_images']}))


if __name__=='__main__':main()
