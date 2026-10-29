"""Dedicated serial, guarded C06+C50 integration runner; no workload persistence."""
import argparse
import copy
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import ModuleType
import uuid

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
VERSION = 'c06_c50_guarded_integration_empty_metadata_v2'
GENERATOR = 'c06_c50_bounded_integration_checks'
EXPECTED_TREE = '138b36fd19ebf6d44f37aa3c73585e9c8be6ff5d'
SOURCE_BINDINGS = {'rbo-fixes-20261023/anonymous_cpp_guard.py': '58941801bad6b72f86546c669107e51cf2b765a806b1d8bb999ac4dfbf310819', 'rbo-fixes-20261023/anonymous_cpp_metadata.py': '83c14f54667d8316a04361a166bedaf9da09a45c0b35322aee445eb1aa9dadbf', 'rbo-fixes-20261023/anonymous_cpp_verify.py': '836054297fe6954e2bbab441f60e9cfd7d0673394a42f2b1aa4de9eb5e392136', 'rbo-fixes-20261023/binary_provenance.py': 'b738e1aefd321965d62009f66739014ab45331ce8d1816ff59a1195e3f98451c', 'rbo-fixes-20261023/c06_c50_bounded_integration_checks.py': '7fb679c0e6b7e4206074584f84db9e749442067cf00902b12dec16e4e0d9d012', 'rbo-fixes-20261023/join_key_checks.py': '32c91d788fd03cbb7035ebfea624f0b62eb012992810944c7450b509020a1acb', 'rbo-fixes-20261023/memory_safe_build.py': 'ba2e6c524bfcf72a2d0507fceda8cd4877ce4a0570bdc1fbd0e56131933363bd', 'rbo-fixes-20261023/native_verify.py': 'e8be177e9e07cb6b6e987be6d65456dcdf5e687056c4b174f022b2bb34140a80', 'rbo-fixes-20261023/optimizer_mode_contract.py': '50ffb02aff06f5b2322cf8b00c87500d24e73580c304120e48ffd76f329c75ad', 'rbo-fixes-20261023/quality_spill_bounds.py': 'f6a303c5aee942311ebac20ff8a01b4dfee809440eccdd5d46265360a399b219', 'rbo-fixes-20261023/verification/C06-C50-bounded-spill-production-source-comparison-20261028.json': '560c8365e4c66f6039d953f72d2bb1be2c147db13e165b1c6bbe0c89b746f514', 'rbo-fixes-20261023/verification/C06-C50-integration-independent-source-review-20261028.json': '530fb5c69460669e57836114a016ab5c9924476095308cf5ba867784c2698516', 'rbo-fixes-20261023/verification/C06-C50-integration-source-preparation-20261028.json': '17473d3cb4501754409d61c1ca225f147a8a1041f0ff0a55f752ba52832d0fa5', 'rbo-fixes-20261023/verification/guarded-fixed-C50-946575513d23-1793145911575205063.jsonl': 'ee458a3f9112bc998787db65e5a0816b265d1a979a7ff8fb5b0dd9448033316f', 'rbo-fixes-20261023/verification_contract.py': '73a7608c2a8ea37395bd2336def4801a3aa77e607324376816fac996242cd952', 'rbo-fixes-20261023/verify_series.py': 'cc07fc271efde3ed8f8377ef86f9560b484591503d7c0198d89e7ca7488d6265', 'rbo-fixes-20261023/window_checks.py': 'c48fc8c06ddc561788de2c28d578b794390649a1b323b87ddd3e11f9af66698e', 'rbo-review-20261021/correlation_oracle_generator.py': '01df4ffff02830784e8aad4d4b072f671f89b8b3a8772a1e0949899551cdb2b9', 'rbo-review-20261021/generate_and_verify.py': 'fa9f950e0d97e784574532f5092ee45ac8cabd8edfba91b9495bc81f8e9afaed', 'rbo-review-20261021/new.conf': '7496d5d173b6dc3a270f2ce7ca2faf78d3bd7864723c200d71e46d77bda7359d', 'rbo-review-20261021/partial_lookup_key_generator.py': '6b1c97b7d675d2a5a8ebc3f0bc2c3bbc08a341e6209e25e989612f8dde745911'}
VECTOR_SHA256 = '391ebefc5cf808b4daae8b59872cdfa9dc5ab24d9c8019ae2d13419d187f64e6'
LIMIT = 256 * 1024
SEALS = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL
SAFE_FIELDS = frozenset(('case','mode','generator_module','node_count','query_syntax',
    'quality_evidence_required','correctness_query_plan_required','correctness_physical_ast_required',
    'verification_contract_version','generator_version','generator_sha256','generator_hash_source',
    'settings_sha256','schema_sha256','fixture_sha256','query_sha256','validation_sha256',
    'setup_request_count','target_request_count','validation_request_count','target_results_checked',
    'expected_rejection_configured','table_service_overrides','optimizer_mode_config_verified',
    'optimizer_mode_configuration','exit_code','row_count','result_sha256','unordered_result_sha256',
    'semantic_oracle_pass','result_set_boundaries_known','result_set_count','timeout',
    'harness_exception','failure_category','failure_stage','diagnostic_sha256','diagnostic_bytes',
    'result_output_format','empty_result_set_witness_version','empty_result_set_metadata_verified',
    'empty_result_set_payload_sha256'))


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def _sources():
    images = {}
    for name, expected in SOURCE_BINDINGS.items():
        path = (PROJECT/name).resolve()
        raw = path.read_bytes()
        if digest(raw) != expected:
            raise ValueError('integration_runner_source_binding_changed')
        images[path] = raw
    return images


def _load(name, path, images):
    path = path.resolve()
    raw = images[path]
    module = ModuleType(name)
    module.__file__ = str(path)
    sys.modules[name] = module
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    module._verification_source_sha256 = digest(raw)
    return module


def _driver(images, worktree):
    sys.path.insert(0, str(ROOT))
    sys.path.insert(0, str(PROJECT/'rbo-review-20261021'))
    for name, directory in (
        ('generate_and_verify','rbo-review-20261021'),
        ('correlation_oracle_generator','rbo-review-20261021'),
        ('partial_lookup_key_generator','rbo-review-20261021'),
        ('optimizer_mode_contract','rbo-fixes-20261023'),
        ('verification_contract','rbo-fixes-20261023'),
        ('binary_provenance','rbo-fixes-20261023'),
        ('quality_spill_bounds','rbo-fixes-20261023')):
        _load(name, PROJECT/directory/(name+'.py'), images)
    driver = _load('native_verify', ROOT/'native_verify.py', images)
    driver.g.BINARY = worktree/'ydb/tests/tools/kqprun/kqprun'
    driver.g.MATH = worktree/'yql/essentials/udfs/common/math/libmath_udf.so'
    return driver


def _generator(images):
    return _load(GENERATOR, ROOT/(GENERATOR+'.py'), images)


def _expected_revision(worktree, expected):
    if not re.fullmatch('[0-9a-f]{40}', expected):
        raise ValueError('invalid_expected_integration_revision')
    actual = subprocess.check_output(['git','rev-parse','HEAD'], cwd=worktree, text=True).strip()
    tree = subprocess.check_output(['git','rev-parse','HEAD^{tree}'], cwd=worktree, text=True).strip()
    if actual != expected or tree != EXPECTED_TREE:
        raise ValueError('integration_revision_or_tree_mismatch')


def _udf_identity(worktree):
    """Bind the existing auxiliary UDF input; this is not its build receipt."""
    path = (worktree/'yql/essentials/udfs/common/math/libmath_udf.so').resolve(strict=True)
    def identity(stat):
        return [stat.st_dev,stat.st_ino,stat.st_size,stat.st_mtime_ns,stat.st_ctime_ns]
    hasher = hashlib.sha256()
    with path.open('rb') as stream:
        before = identity(os.fstat(stream.fileno()))
        for block in iter(lambda:stream.read(1024*1024),b''):
            hasher.update(block)
        after = identity(os.fstat(stream.fileno()))
    if before!=after or after!=identity(path.stat()):
        raise ValueError('auxiliary_udf_changed_during_fingerprint')
    return {'resolved_path':str(path),'file_identity':after,'sha256':hasher.hexdigest(),
            'separate_successful_build_receipt_verified':False}


def _emit(value):
    sys.stdout.write(json.dumps(value, sort_keys=True, separators=(',', ':'))+'\n')
    sys.stdout.flush()


def _worker(spec):
    """All database invocations occur in this guarded process, one at a time."""
    expected_keys = {'version','run_id','nonce','worktree','revision','build_receipt','receipt_sha256',
                     'runner_source_sha256','source_bindings','vector_sha256','tree'}
    if (not isinstance(spec,dict) or set(spec)!=expected_keys or spec['version']!=VERSION
        or spec['source_bindings']!=SOURCE_BINDINGS or spec['vector_sha256']!=VECTOR_SHA256
        or spec['tree']!=EXPECTED_TREE or not re.fullmatch('[0-9a-f]{32}',spec['run_id'])
        or not re.fullmatch('[0-9a-f]{64}',spec['nonce'])
        or digest(Path(__file__).read_bytes())!=spec['runner_source_sha256']):
        raise ValueError('integration_worker_manifest_mismatch')
    cgroup = Path('/proc/self/cgroup').read_bytes()
    if not re.search(rb'/rbo-direct-[0-9a-f]+\.scope(?:\n|$)',cgroup):
        raise ValueError('integration_worker_requires_guard_scope')
    identity_hash = digest(canonical(spec))
    worktree = Path(spec['worktree']).resolve(strict=True)
    receipt = Path(spec['build_receipt']).resolve(strict=True)
    if digest(receipt.read_bytes())!=spec['receipt_sha256']:
        raise ValueError('integration_worker_receipt_changed')
    _expected_revision(worktree,spec['revision'])
    images = _sources()
    driver = _driver(images,worktree)
    generator = _generator(images)
    identity = generator.initialize_integration(worktree,receipt,driver)
    if (identity['revision']!=spec['revision'] or identity['tree']!=EXPECTED_TREE
        or identity['build_receipt_sha256']!=spec['receipt_sha256']):
        raise ValueError('integration_initialized_scope_differs')
    vector = generator.required_record_vector()
    if digest(canonical(vector))!=VECTOR_SHA256:
        raise ValueError('integration_worker_vector_changed')
    provenance = copy.deepcopy(generator._scope['provenance'])
    auxiliary_udf = _udf_identity(worktree)
    _emit({'kind':'start','run_id':spec['run_id'],'nonce':spec['nonce'],
           'identity_sha256':identity_hash,'worker_guard_scope_sha256':digest(cgroup),
           'integration_scope':identity,'auxiliary_udf':auxiliary_udf})
    completed = 0
    for index, cell in enumerate(vector):
        try:
            raw = driver.run_generated(generator,cell['case'],'new',24)
            if not isinstance(raw,dict): raise ValueError('non_object_native_record')
            record = {key:value for key,value in raw.items() if key in SAFE_FIELDS}
        except Exception as error:
            record = {'semantic_oracle_pass':False,'harness_exception':type(error).__name__}
        record.update(case=cell['case'],mode='new',rows=24,node_count=4,
                      repetition=cell['repetition'],bug='C06+C50',phase='integration',
                      revision=spec['revision'],integration_scope=identity,
                      auxiliary_udf=auxiliary_udf,
                      integration_run_id=spec['run_id'],integration_nonce=spec['nonce'],
                      integration_record_index=index,**copy.deepcopy(provenance))
        record['binary_provenance_required'] = True
        validation_interruption = None
        try:
            valid = generator.validate_record(record,driver.g.BINARY)
            record['integration_record_validation_pass'] = True
            if not valid: record['semantic_oracle_pass'] = False
        except BaseException as error:
            record['observed_semantic_oracle_pass'] = record.get('semantic_oracle_pass') is True
            record['semantic_oracle_pass'] = False
            record['integration_record_validation_pass'] = False
            record['integration_validation_failure_type'] = type(error).__name__
            if not isinstance(error, Exception) or isinstance(error, InterruptedError):
                validation_interruption = error
        # Persisting this envelope later never depends on the validation result.
        _emit({'kind':'record','run_id':spec['run_id'],'nonce':spec['nonce'],
               'identity_sha256':identity_hash,'index':index,'record':record})
        completed += 1
        if validation_interruption is not None:
            raise validation_interruption
        try:
            generator.integration_identity()
            if _udf_identity(worktree)!=auxiliary_udf:
                raise ValueError('auxiliary_udf_changed')
        except Exception as error:
            _emit({'kind':'stop','run_id':spec['run_id'],'nonce':spec['nonce'],
                   'identity_sha256':identity_hash,'completed_records':completed,
                   'failure_type':type(error).__name__})
            return 2
    _emit({'kind':'complete','run_id':spec['run_id'],'nonce':spec['nonce'],
           'identity_sha256':identity_hash,'completed_records':completed})
    return 0


def _read_manifest(path):
    if not re.fullmatch('/proc/self/fd/[0-9]+',path):
        raise ValueError('worker_manifest_must_be_anonymous')
    fd = int(path.rsplit('/',1)[1])
    if ((fcntl.fcntl(fd,fcntl.F_GETFL)&os.O_ACCMODE)!=os.O_RDONLY
        or fcntl.fcntl(fd,fcntl.F_GET_SEALS)!=SEALS
        or not os.readlink(path).startswith('/memfd:')):
        raise ValueError('worker_manifest_not_sealed_readonly_memfd')
    raw = os.pread(fd,64*1024+1,0)
    if len(raw)>64*1024: raise ValueError('worker_manifest_too_large')
    return json.loads(raw)


def _manifest_fd(raw):
    fd = os.memfd_create('c06-c50-run-manifest',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
    try:
        if os.write(fd,raw)!=len(raw): raise OSError('short_manifest_write')
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,SEALS)
        return os.open('/proc/self/fd/'+str(fd),os.O_RDONLY|os.O_CLOEXEC)
    finally:
        os.close(fd)


def _parse_slice(stdout,spec):
    """Retain a valid sanitized prefix even if later output is incomplete."""
    records=[];start=None;complete=False;error=None
    expected_hash=digest(canonical(spec))
    for line in stdout.splitlines():
        try:
            item=json.loads(line)
            if (not isinstance(item,dict) or item.get('run_id')!=spec['run_id']
                or item.get('nonce')!=spec['nonce'] or item.get('identity_sha256')!=expected_hash):
                raise ValueError('slice_identity_mismatch')
            kind=item.get('kind')
            if kind=='start':
                if start is not None or records or complete: raise ValueError('duplicate_slice_start')
                start=item
            elif kind=='record':
                if start is None or complete or item.get('index')!=len(records) or not isinstance(item.get('record'),dict):
                    raise ValueError('slice_record_order')
                record=item['record']
                if (record.get('integration_run_id')!=spec['run_id'] or record.get('integration_nonce')!=spec['nonce']
                    or record.get('integration_record_index')!=len(records)):
                    raise ValueError('slice_record_identity')
                records.append(record)
            elif kind=='complete':
                if start is None or complete or item.get('completed_records')!=len(records): raise ValueError('slice_completion_mismatch')
                complete=True
            elif kind=='stop':
                error='WorkerStopped';break
            else:
                raise ValueError('unknown_slice_item')
        except Exception as exc:
            error=type(exc).__name__;break
    return records,start,complete,error


def _reserve(path):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_CLOEXEC,0o600)
    os.close(fd)


def _write(path,value):
    Path(path).write_text(json.dumps(value,sort_keys=True,indent=2)+'\n')


def run(worktree,expected_revision,build_receipt):
    """Parent path; actual work is launched only inside the unchanged guard."""
    worktree=Path(worktree).resolve(strict=True);receipt=Path(build_receipt).resolve(strict=True)
    images=_sources()
    sys.path.insert(0,str(ROOT))
    _load('memory_safe_build',ROOT/'memory_safe_build.py',images)
    guard=_load('anonymous_cpp_guard',ROOT/'anonymous_cpp_guard.py',images)
    _load('anonymous_cpp_metadata',ROOT/'anonymous_cpp_metadata.py',images)
    direct=_load('anonymous_cpp_verify',ROOT/'anonymous_cpp_verify.py',images)
    with direct.serial_slot():
        _expected_revision(worktree,expected_revision)
        run_id=uuid.uuid4().hex
        spec={'version':VERSION,'run_id':run_id,'nonce':uuid.uuid4().hex+uuid.uuid4().hex,
              'worktree':str(worktree),'revision':expected_revision,'build_receipt':str(receipt),
              'receipt_sha256':digest(receipt.read_bytes()),'runner_source_sha256':digest(Path(__file__).read_bytes()),
              'source_bindings':dict(SOURCE_BINDINGS),'vector_sha256':VECTOR_SHA256,'tree':EXPECTED_TREE}
        prefix=ROOT/'verification'/('integration-C06-C50-'+expected_revision[:12]+'-'+run_id)
        paths={key:Path(str(prefix)+suffix) for key,suffix in
               (('memory','-memory.json'),('records','-records.jsonl'),('summary','-summary.json'))}
        for path in paths.values():_reserve(path)
        command=[sys.executable,'-B',str(Path(__file__).resolve()),'--worker','@FD:manifest@']
        fd=None;guard_exception=None;stdout=b''
        memory={'exit_code':None,'cleanup_complete':False,'watchdog_stopped':False,
                'max_scope_gib':24,'min_host_available_gib':36}
        try:
            fd=_manifest_fd(canonical(spec))
            memory,stdout=guard.run_anonymous(command,PROJECT,{'manifest':fd},timeout=16*120+300,
                max_scope_gib=24,min_host_gib=36,retain_stdout=LIMIT,compiler_environment=False,
                metrics_path=paths['memory'])
        except BaseException as error:
            # No false cleanup claim if the shared guard itself cannot return.
            guard_exception=type(error).__name__
        finally:
            if fd is not None:os.close(fd)
        records,start,complete,parse_error=_parse_slice(stdout,spec)
        stream_verified=(memory.get('stdout_sha256')==digest(stdout)
                         and memory.get('stdout_bytes')==len(stdout) and memory.get('stdout_overflow') is False)
        identity_hash=digest(canonical(spec))
        guard_binding={'integration_run_id':run_id,'integration_nonce':spec['nonce'],
            'integration_identity_sha256':identity_hash,'integration_command_template_sha256':digest(canonical(command)),
            'integration_vector_sha256':VECTOR_SHA256,'integration_worker_source_sha256':spec['runner_source_sha256'],
            'integration_guarded_stdout_sha256':digest(stdout),'integration_guarded_stdout_bytes':len(stdout)}
        memory.update(guard_binding)
        _write(paths['memory'],memory)
        memory_hash=digest(paths['memory'].read_bytes())
        guard_pass=(type(memory.get('exit_code')) is int and memory['exit_code']==0
                    and memory.get('cleanup_complete') is True and memory.get('watchdog_stopped') is False
                    and memory.get('max_scope_gib')==24 and memory.get('min_host_available_gib')==36
                    and memory.get('scope_membership_accounting_available') is True and stream_verified
                    and guard_exception is None)
        for record in records:
            record.update(native_batch_guard_pass=guard_pass,native_batch_memory_evidence=str(paths['memory']),
                          native_batch_memory_sha256=memory_hash,
                          integration_guard_binding_sha256=digest(canonical(guard_binding)))
        # Save every completed cell, including rejected/failed cells and prefixes,
        # before final provenance/vector validation can fail or be interrupted.
        paths['records'].write_text(''.join(json.dumps(x,sort_keys=True)+'\n' for x in records))
        summary={'schema':VERSION,'run_id':run_id,'identity_sha256':identity_hash,'revision':expected_revision,
            'tree':EXPECTED_TREE,'source_bindings':dict(SOURCE_BINDINGS),'record_count':len(records),
            'expected_record_count':16,'complete_worker_slice':complete,'slice_error_type':parse_error,
            'guard_exception_type':guard_exception,'worker_source_sha256':spec['runner_source_sha256'],
            'guard_pass':guard_pass,'guard_binding':guard_binding,'memory_path':str(paths['memory']),
            'memory_sha256':memory_hash,'records_path':str(paths['records']),
            'records_sha256':digest(paths['records'].read_bytes()),'bounded_integration_semantics_pass':False,
            'standalone_fix_admission_changed':False,'original_failure_records_changed':False,
            'final_distribution_proof':False,'rendered_workloads_or_raw_results_persisted':False}
        _write(paths['summary'],summary)
        if guard_pass and complete and parse_error is None and len(records)==16 and start is not None:
            try:
                _expected_revision(worktree,expected_revision)
                if digest(receipt.read_bytes())!=spec['receipt_sha256']:raise ValueError('receipt_changed')
                if digest(Path(__file__).read_bytes())!=spec['runner_source_sha256']:raise ValueError('runner_source_changed')
                fresh=_sources()
                driver=_driver(fresh,worktree)
                generator=_generator(fresh)
                identity=generator.initialize_integration(worktree,receipt,driver)
                if identity!=start.get('integration_scope'):raise ValueError('scope_changed_after_guard')
                auxiliary_udf=_udf_identity(worktree)
                if (auxiliary_udf!=start.get('auxiliary_udf')
                    or any(x.get('auxiliary_udf')!=auxiliary_udf for x in records)):
                    raise ValueError('auxiliary_udf_changed_after_guard')
                # These are checked against this call's returned guard and slice,
                # rather than accepting a caller-selected old guard document.
                if (digest(canonical(generator.required_record_vector()))!=VECTOR_SHA256
                    or any(x.get('integration_guard_binding_sha256')!=digest(canonical(guard_binding)) for x in records)
                    or memory.get('integration_identity_sha256')!=identity_hash):
                    raise ValueError('guard_slice_binding_changed')
                result=generator.validate_complete(records)
                summary['validation']=result
                summary['bounded_integration_semantics_pass']=result['bounded_integration_semantics_pass']
            except Exception as error:
                summary['final_validation_failure_type']=type(error).__name__
        summary['status']='verified_bounded_integration' if summary['bounded_integration_semantics_pass'] else 'integration_evidence_failed_or_incomplete'
        _write(paths['summary'],summary)
        return summary


def main():
    if len(sys.argv)==3 and sys.argv[1]=='--worker':
        try:return _worker(_read_manifest(sys.argv[2]))
        except Exception as error:
            _emit({'kind':'initialization_failure','failure_type':type(error).__name__})
            return 2
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worktree',type=Path,required=True)
    parser.add_argument('--expected-revision',required=True)
    parser.add_argument('--build-receipt',type=Path,required=True)
    args=parser.parse_args()
    try:
        result=run(args.worktree,args.expected_revision,args.build_receipt)
        print(json.dumps({'status':result['status'],'run_id':result['run_id'],
                          'record_count':result['record_count'],'records_path':result['records_path']}),flush=True)
        return 0 if result['bounded_integration_semantics_pass'] else 2
    except Exception as error:
        print(json.dumps({'status':'integration_runner_rejected','failure_type':type(error).__name__}),flush=True)
        return 2

if __name__=='__main__':raise SystemExit(main())
