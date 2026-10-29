"""Prepare and execute anonymous direct-production C++ quality contracts.

The CLI is deliberately separate from the active correctness series. It refuses
to start until work-state.phase is direct_cpp_verification, sources are frozen,
and no other verification driver is alive. It never updates a case to fully
verified: direct IR contracts are narrower than end-to-end query semantics.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from types import ModuleType

from anonymous_cpp_guard import NATIVE_EXECUTABLE_SEALS, run_anonymous
from anonymous_cpp_metadata import (VERSION as CLOSURE_VERSION, MetadataError, digest_file, issue_receipt,
                                    production_build_arguments, production_graph_arguments, production_configuration,
                                    source_revision, verify_receipt)

HERE = Path(__file__).resolve().parent
GENERATORS = {'C31': 'c31_combined_cpp_generator', 'C47': 'c47_combined_cpp_generator',
              'Q11': 'quality_union_cpp_generator',
              'Q09': 'quality_statistics_cpp_generator', 'Q20': 'quality_statistics_cpp_generator',
              'Q01': 'quality_range_runtime_cpp_generator', 'Q14': 'quality_rule_cpp_generator', 'Q16': 'quality_rule_cpp_generator',
              'Q15': 'quality_partition_cpp_checks',
              'Q18': 'quality_cpp_contract_generator', 'Q19': 'quality_cpp_contract_generator',
              'Q25': 'quality_map_contract_generator', 'Q27': 'quality_ast_collection_generator',
              'Q28': 'quality_scheduler_cpp_checks'}


def _json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def _closure_profile(bug):
    return 'kqp_runtime' if bug == 'Q01' else 'ast' if bug == 'Q27' else 'rbo'


def _expected_revision(bug, phase, worktree):
    if bug in ('C31', 'C47', 'Q01', 'Q11', 'Q15') and phase != 'fixed':
        raise MetadataError('new_runtime_or_helper_requires_fixed_only_admission')
    case = json.loads((HERE / 'cases' / (bug + '.json')).read_text())
    parent = subprocess.check_output(['git', 'rev-parse', case['commit'] + '^'], cwd=worktree, text=True).strip()
    if parent != case['base']:
        raise MetadataError('finding_branch_is_not_independent_main_child')
    return case['base'] if phase == 'baseline' else case['commit']


@contextmanager
def serial_slot():
    state = json.loads((HERE / 'work-state.json').read_text())
    if not state.get('production_sources_frozen') or state.get('phase') != 'direct_cpp_verification':
        raise MetadataError('direct_cpp_slot_not_released_by_active_series')
    lock = (HERE / 'verification' / 'direct-cpp.lock').open('a+')
    try:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise MetadataError('another_direct_cpp_verifier_is_active') from None
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit() or int(entry.name) == os.getpid():
                continue
            try:
                arguments = (entry / 'cmdline').read_bytes().split(b'\0')
            except OSError:
                continue
            if any(Path(os.fsdecode(arg)).name in ('verify_series.py', 'native_verify.py',
                                                   'anonymous_cpp_verify.py') for arg in arguments[1:] if arg):
                raise MetadataError('another_verification_process_is_active')
        yield state
    finally:
        lock.close()


def _write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')
    temporary.replace(path)


def prepare(args, state):
    from memory_safe_build import run_build
    expected = _expected_revision(args.bug, args.phase, args.worktree)
    if source_revision(args.worktree) != expected:
        raise MetadataError('prepare_requires_selected_clean_source_revision')
    workers = state.get('authorized_build_workers')
    if workers != 8:
        raise MetadataError('approved_eight_worker_verification_setting_required')
    profile = _closure_profile(args.bug)
    directory = HERE / 'verification' / 'cpp-closures' / (expected[:12] + '-' + str(time.time_ns()))
    directory.mkdir(parents=True, exist_ok=False)
    export_root, graph_path = directory / 'outputs', directory / 'production-graph.json'
    arguments = production_build_arguments(profile, export_root, graph_path,
                                          export_objects=args.bug not in ('C31', 'C47'))
    if any(any(char.isspace() for char in arg) for arg in arguments):
        raise MetadataError('build_guard_argument_transport_rejects_whitespace_paths')
    configuration = production_configuration(arguments, workers)
    start = time.monotonic()
    build, memory = run_build(args.worktree, ' '.join(arguments), workers)
    record = {'build_record_version': 'production_cpp_build_v2',
              'revision': expected, 'revision_before': expected,
              'revision_after': source_revision(args.worktree),
              'profile': profile, 'exit_code': build.returncode,
              'export_root': str(export_root), 'graph_path': str(graph_path),
              'target': arguments[0], 'elapsed_seconds': round(time.monotonic() - start, 3),
              'diagnostic_sha256': hashlib.sha256((build.stdout + build.stderr).encode()).hexdigest(),
              'memory': memory, 'explicit_build_workers': workers,
              'broad_object_export': args.bug not in ('C31', 'C47'),
              'configuration': configuration}
    _write_json(directory / 'build.json', record)
    if build.returncode:
        return {'status': 'production_closure_build_failed', 'build_record': str(directory / 'build.json')}, 1
    graph_record, graph_code = export_graph(args, directory / 'build.json')
    if graph_code:
        return graph_record, graph_code
    return capture_closure(args, directory / 'build.json')


def export_graph(args, build_record_path):
    from memory_safe_build import run_build
    expected = _expected_revision(args.bug, args.phase, args.worktree)
    build_record_path = Path(build_record_path).resolve()
    raw_build = build_record_path.read_bytes()
    build = json.loads(raw_build)
    profile = _closure_profile(args.bug)
    configuration = build.get('configuration', {})
    workers = configuration.get('workers')
    graph_path = Path(build['graph_path']).resolve()
    export_root = Path(build['export_root']).resolve()
    if type(build.get('broad_object_export')) is not bool:
        raise MetadataError('graph_export_requires_recorded_object_export_policy')
    arguments = production_build_arguments(profile, export_root, graph_path,
                                           export_objects=build['broad_object_export'])
    if (build.get('build_record_version') != 'production_cpp_build_v2'
        or build.get('exit_code') != 0 or build.get('memory', {}).get('watchdog_stopped') is not False
        or build.get('revision') != expected or build.get('revision_before') != expected
        or build.get('revision_after') != expected or build.get('profile') != profile
        or build.get('target') != arguments[0] or workers != 8
        or configuration != production_configuration(arguments, workers)
        or source_revision(args.worktree) != expected):
        raise MetadataError('graph_export_requires_matching_successful_build_configuration')
    if build_record_path != graph_path.parent / 'build.json':
        raise MetadataError('graph_export_build_record_location_mismatch')
    command = production_graph_arguments(arguments, graph_path)
    if any(any(char.isspace() for char in arg) for arg in command):
        raise MetadataError('graph_export_argument_transport_rejects_whitespace_paths')
    start = time.monotonic()
    result, memory = run_build(args.worktree, ' '.join(command), workers)
    record = {'schema_version': 'production_graph_export_v1', 'exit_code': result.returncode,
              'profile': profile, 'revision_before': expected,
              'revision_after': source_revision(args.worktree), 'graph_path': str(graph_path),
              'configuration': configuration, 'arguments': command,
              'build_record_sha256': hashlib.sha256(raw_build).hexdigest(), 'memory': memory,
              'elapsed_seconds': round(time.monotonic() - start, 3),
              'diagnostic_sha256': hashlib.sha256((result.stdout + result.stderr).encode()).hexdigest()}
    if (result.returncode == 0 and not memory.get('watchdog_stopped')
        and record['revision_after'] == expected and build_record_path.read_bytes() == raw_build
        and graph_path.is_file()):
        record['graph'] = digest_file(graph_path)
    _write_json(graph_path.parent / 'graph-export.json', record)
    if 'graph' not in record:
        return {'status': 'production_graph_export_failed',
                'build_record': str(build_record_path)}, 1
    return {'status': 'production_graph_exported', 'graph': str(graph_path)}, 0


def capture_closure(args, build_record_path):
    """Retry only metadata adaptation after a successful saved production build."""
    expected = _expected_revision(args.bug, args.phase, args.worktree)
    record = json.loads(Path(build_record_path).read_bytes())
    profile = _closure_profile(args.bug)
    graph_path, export_root = Path(record['graph_path']), Path(record['export_root'])
    resources = {}
    for value in args.resource:
        key, sep, path = value.partition('=')
        if not sep or not re.fullmatch(r'[A-Za-z0-9_:-]+', key) or not Path(path).is_dir():
            raise MetadataError('invalid_explicit_graph_resource_mapping')
        resources[key] = str(Path(path).resolve())
    receipt = issue_receipt(args.worktree, expected, record, graph_path, export_root, profile, resources)
    path = Path(build_record_path).parent / 'receipt.json'
    _write_json(path, receipt)
    return {'status': 'production_closure_prepared', 'revision': expected, 'receipt': str(path),
            'receipt_sha256': digest_file(path)['sha256'], 'input_count': len(receipt['files']),
            'generated_contracts_compiled': False}, 0


def _tool(receipt, name):
    compiler = Path(receipt['metadata']['compile_argv'][0]).resolve()
    candidate = compiler.parent / name
    if not candidate.is_file():
        raise MetadataError('required_object_inspection_tool_missing')
    return candidate


def _read_tool(receipt, command, worktree, fds=None, limit=16 * 1024 * 1024):
    record, output = run_anonymous(command, worktree, fds or {}, timeout=120,
                                   max_scope_gib=8, retain_stdout=limit)
    if record['exit_code'] or record['stdout_overflow'] or not record['cleanup_complete']:
        raise MetadataError('object_inspection_did_not_complete')
    return output.decode('utf-8', errors='strict'), record


def _q19_object_path(receipt, stem):
    matches = [path for path in receipt['files']
               if Path(path).name in (stem + '.o', stem + '.pic.o', stem + '.abi_stable.o')]
    if len(matches) != 1:
        raise MetadataError('required_q19_production_object_is_ambiguous')
    return matches[0]


def q19_wrapping(receipt, worktree):
    files = receipt['files']
    definitions = _q19_object_path(receipt, 'kqp_column_statistics_utils.cpp')
    caller = _q19_object_path(receipt, 'kqp_rbo_transformer.cpp')
    nm, readelf = _tool(receipt, 'llvm-nm'), _tool(receipt, 'llvm-readelf')
    symbols_text, nm_record = _read_tool(receipt, [str(nm), '--defined-only', '--format=posix', definitions], worktree)
    symbols = {}
    for line in symbols_text.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[1] not in ('T', 'W') or 'AddStatRequest' not in fields[0]:
            continue
        symbol = fields[0]
        if not re.fullmatch(r'_Z[A-Za-z0-9_]+', symbol):
            continue
        key = 'tuple' if 'TMultiColumnStatistics' in symbol else 'single' if 'TColumnStatistics' in symbol else None
        if key is None or key in symbols:
            raise MetadataError('q19_request_symbol_overloads_are_ambiguous')
        symbols[key] = symbol
    if set(symbols) != {'single', 'tuple'}:
        raise MetadataError('both_q19_request_symbols_are_required')
    relocs, reloc_record = _read_tool(receipt, [str(readelf), '--relocs', '--wide', caller], worktree)
    if not all(symbol in relocs for symbol in symbols.values()):
        raise MetadataError('q19_actual_caller_relocations_not_confirmed')
    return symbols, {'definition_object_sha256': files[definitions]['sha256'],
                     'caller_object_sha256': files[caller]['sha256'],
                     'request_symbols': dict(symbols),
                     'request_symbol_sha256': {key: hashlib.sha256(value.encode()).hexdigest() for key, value in symbols.items()},
                     'both_caller_relocations_verified': True, 'nm': nm_record, 'relocations': reloc_record,
                     'inspection_tools': {str(tool): digest_file(tool) for tool in (nm, readelf)}}


def render_suite(bug, worktree, revision, wrapper_symbols=None):
    if bug == 'Q11':
        from quality_union_cpp_contract import render_suite as render_union_suite
        return render_union_suite(revision)
    if bug in ('C31', 'C47'):
        from correctness_cpp_contract import render_suite as render_correctness_suite
        return render_correctness_suite(bug, revision)
    if bug in PAIRED_DIRECT_BUGS:
        module, generator_hash, _ = _current_pair_contract(bug)
    else:
        module = importlib.import_module(GENERATORS[bug])
        generator_hash = None
    if bug == 'Q27':
        source = module.render_cpp_suite(worktree, revision)
        count = len(module.specs())
    elif bug in ('Q01', 'Q15', 'Q25', 'Q28'):
        source = module.render_cpp_suite()
        count = len(module.cases())
    else:
        selected = [case for case in module.cases() if case.startswith(bug.lower() + '_')]
        count = len(selected)
        source = module.render_cpp_suite(bug, wrapper_symbols) if bug in ('Q18', 'Q19') else module.render_cpp_suite(bug)
    if count < 1:
        raise MetadataError('empty_generated_suite')
    return source.encode(), count, generator_hash or digest_file(module.__file__)['sha256']


def parse_verdicts(stdout, count, exit_code, bug):
    if bug == 'Q11':
        from quality_union_cpp_contract import parse_verdicts as parse_union_verdicts
        return parse_union_verdicts(stdout, count, exit_code)
    if bug in ('C31', 'C47'):
        from correctness_cpp_contract import parse_verdicts as parse_correctness_verdicts
        return parse_correctness_verdicts(stdout, count, exit_code, bug=bug)
    if bug == 'Q01':
        from quality_range_runtime_contract import parse_verdicts as parse_runtime_verdicts
        return parse_runtime_verdicts(stdout, count, exit_code)
    if not stdout or len(stdout) > 1024 * 1024:
        raise MetadataError('missing_or_oversized_direct_verdicts')
    records = []
    for line in stdout.splitlines():
        value = json.loads(line)
        allowed = {'case_index', 'checks_executed', 'checks', 'passed'}
        if bug == 'Q27':
            allowed |= {'entries', 'unique_visible_nodes'}
        if bug == 'Q28':
            allowed |= {'output_refreshes', 'rule_attempts', 'candidate_visits',
                        'control_output_refreshes', 'control_rule_attempts', 'control_candidate_visits'}
        if not isinstance(value, dict) or set(value) - allowed:
            raise MetadataError('unexpected_direct_verdict_fields')
        if type(value.get('case_index')) is not int or type(value.get('passed')) is not bool:
            raise MetadataError('invalid_direct_verdict_types')
        checks = value.get('checks_executed', value.get('checks'))
        if type(checks) is not int or checks <= 0:
            raise MetadataError('direct_case_has_no_executed_assertions')
        if 'checks' in value and 'checks_executed' in value:
            raise MetadataError('ambiguous_direct_assertion_count')
        if bug == 'Q27' and any(type(value.get(key)) is not int or value[key] < 0
                               for key in ('entries', 'unique_visible_nodes')):
            raise MetadataError('missing_collector_work_measurement')
        if bug == 'Q28' and any(type(value.get(key)) is not int or value[key] < 0 for key in
                               ('output_refreshes', 'rule_attempts', 'candidate_visits',
                                'control_output_refreshes', 'control_rule_attempts', 'control_candidate_visits')):
            raise MetadataError('missing_scheduler_work_measurement')
        records.append(dict(value, checks_executed=checks))
        records[-1].pop('checks', None)
    if len(records) != count or sorted(value['case_index'] for value in records) != list(range(count)):
        raise MetadataError('incomplete_or_duplicate_direct_cases')
    failures = sum(not value['passed'] for value in records)
    if exit_code != (1 if failures else 0):
        raise MetadataError('direct_exit_status_disagrees_with_case_verdicts')
    records = sorted(records, key=lambda value: value['case_index'])
    if bug in ('Q09', 'Q20'):
        from quality_statistics_cpp_contract import check_records
        check_records(records, bug)
    return records


def _memfd(name, data=b'', executable=False):
    fd = os.memfd_create(name, os.MFD_CLOEXEC | (os.MFD_ALLOW_SEALING if executable else 0))
    if data:
        with os.fdopen(os.dup(fd), 'wb') as stream:
            stream.write(data)
    os.lseek(fd, 0, os.SEEK_SET)
    if executable:
        os.fchmod(fd, 0o700)
    return fd


def _seal_native_executable(fds):
    """Replace our sole writable executable owner with a sealed read-only FD."""
    # Keep signal cancellation outside the ownership-transfer window.
    previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
    try:
        writable = fds['executable']
        if ((fcntl.fcntl(writable, fcntl.F_GETFL) & os.O_ACCMODE) != os.O_RDWR
            or not os.readlink('/proc/self/fd/' + str(writable)).startswith('/memfd:')):
            raise MetadataError('anonymous_executable_requires_writable_memfd_owner')
        os.fchmod(writable, 0o700)
        fcntl.fcntl(writable, fcntl.F_ADD_SEALS, NATIVE_EXECUTABLE_SEALS)
        readonly = os.open('/proc/self/fd/' + str(writable), os.O_RDONLY | os.O_CLOEXEC)
        try:
            before, after = os.fstat(writable), os.fstat(readonly)
            if ((before.st_dev, before.st_ino, before.st_size) != (after.st_dev, after.st_ino, after.st_size)
                or (fcntl.fcntl(readonly, fcntl.F_GETFL) & os.O_ACCMODE) != os.O_RDONLY
                or fcntl.fcntl(readonly, fcntl.F_GET_SEALS) != NATIVE_EXECUTABLE_SEALS):
                raise MetadataError('anonymous_executable_read_only_reopen_mismatch')
        except BaseException:
            os.close(readonly)
            raise
        # Link scope cleanup has already closed its output and driver descriptors.
        # Replace the owning mapping before closing its old writable descriptor.
        fds['executable'] = readonly
        os.close(writable)
        return {'version': 'sealed_read_only_memfd_v1', 'same_inode': True,
                'write_grow_shrink_and_seal_seals': True, 'writable_owner_closed': True}
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)


def _fd_digest(fd):
    h = hashlib.sha256()
    offset = 0
    while True:
        data = os.pread(fd, 1024 * 1024, offset)
        if not data:
            return {'sha256': h.hexdigest(), 'size_bytes': offset}
        h.update(data)
        offset += len(data)


def _stdout_link_arguments(arguments):
    """Bypass LLD's path writability probe without creating an output file."""
    outputs = [index for index, value in enumerate(arguments) if value == '-o']
    if (len(outputs) != 1 or outputs[0] + 1 >= len(arguments)
        or arguments[outputs[0] + 1] != '@FD:executable@'
        or arguments.count('@FD:executable@') != 1):
        raise MetadataError('anonymous_link_output_transport_requires_audit')
    adapted = list(arguments)
    adapted[outputs[0] + 1] = '-'
    return adapted, {'transport': 'stdout_to_executable_memfd',
                     'original_argv_sha256': _json_digest(arguments),
                     'adapted_argv_template_sha256': _json_digest(adapted)}


def _archive_response_arguments(arguments, fds, closure_files):
    """Keep archive order and group boundaries while avoiding driver temp files."""
    adapted, responses = [], []
    index = 0
    while index < len(arguments):
        if not (arguments[index].startswith('/') and arguments[index].endswith('.a')):
            adapted.append(arguments[index])
            index += 1
            continue
        start = index
        while index < len(arguments) and arguments[index].startswith('/') and arguments[index].endswith('.a'):
            index += 1
        archives = arguments[start:index]
        if any(path not in closure_files or '\0' in path for path in archives):
            raise MetadataError('anonymous_archive_response_has_unbound_input')
        # LLVM's GNU response tokenizer recognizes double quotes and backslash escapes.
        data = ''.join('"' + path.replace('\\', '\\\\').replace('"', '\\"') + '"\n'
                       for path in archives).encode()
        name = 'link_archives_' + str(len(responses))
        if name in fds:
            raise MetadataError('anonymous_archive_response_descriptor_collision')
        fds[name] = _memfd('rbo-link-archives', data)
        content = _fd_digest(fds[name])
        if content != {'sha256': hashlib.sha256(data).hexdigest(), 'size_bytes': len(data)}:
            raise MetadataError('anonymous_archive_response_content_mismatch')
        responses.append({'descriptor': name, 'argument_start': start, 'archive_count': len(archives),
                          'archive_sequence_sha256': _json_digest(archives), 'content': content})
        adapted.append('-Wl,@@FD:' + name + '@')
    return adapted, {'original_argv_sha256': _json_digest(arguments),
                     'adapted_argv_template_sha256': _json_digest(adapted), 'responses': responses}


def execute(args):
    revision = _expected_revision(args.bug, args.phase, args.worktree)
    receipt_path = args.receipt.resolve()
    raw_receipt = receipt_path.read_bytes()
    receipt = json.loads(raw_receipt)
    try:
        verify_receipt(receipt, args.worktree, revision)
    except MetadataError as exc:
        exc.verification_stage = 'initial_receipt'
        raise
    expected_profile = _closure_profile(args.bug)
    if receipt['metadata']['profile'] != expected_profile:
        raise MetadataError('wrong_production_closure_for_generator')
    symbols, wrap_evidence = q19_wrapping(receipt, args.worktree) if args.bug == 'Q19' else (None, None)
    source, count, generator_hash = render_suite(args.bug, args.worktree, revision, symbols)
    fds = {'source': _memfd('rbo-contract-source', source), 'object': _memfd('rbo-contract-object'),
           'executable': _memfd('rbo-contract-executable', executable=True)}
    evidence = {'schema_version': 'anonymous_direct_cpp_v1', 'bug': args.bug, 'phase': args.phase,
                'revision': revision, 'generator_module': GENERATORS[args.bug],
                'generator_sha256': generator_hash, 'generated_source_sha256': hashlib.sha256(source).hexdigest(),
                'generated_source_bytes': len(source), 'case_count': count,
                'closure_receipt_path': str(receipt_path), 'closure_receipt_sha256': hashlib.sha256(raw_receipt).hexdigest(),
                'closure_provenance_verified': True, 'wrapping': wrap_evidence,
                'generated_files_persisted': False, 'query_semantics_verified': False,
                'quality_improvement_verified': False,
                'proof_scope': ('linked_production_runtime_lifecycle_contracts' if args.bug == 'Q01' else
                                'source_extracted_helper' if args.bug == 'Q27' else 'linked_production_local_contracts')}
    del source
    verification_stage = 'compile'
    try:
        if args.bug in PAIRED_DIRECT_BUGS:
            _, current_hash, vector = _current_pair_contract(args.bug)
            if current_hash != generator_hash or len(vector['case_names']) != count:
                raise MetadataError('direct_pair_generator_changed_before_compile')
            evidence['direct_case_vector_sha256'] = _json_digest(vector)
        if args.bug in ('C31', 'C47'):
            from correctness_cpp_contract import profile_digest, vector_evidence
            evidence['direct_contract_profile_sha256'] = profile_digest(args.bug, revision)
            evidence.update(vector_evidence(args.bug, revision))
        if args.bug == 'Q11':
            from quality_union_cpp_contract import profile_digest as union_profile_digest
            evidence['direct_contract_profile_sha256'] = union_profile_digest(revision)
        compile_record, _ = run_anonymous(receipt['metadata']['compile_argv'], receipt['metadata']['compile_cwd'], fds,
                                          stdout_fd='object', timeout=args.compile_timeout,
                                          environment=receipt['metadata']['compile_environment'])
        evidence['compile'] = compile_record
        if compile_record.get('environment_sha256') != _json_digest(receipt['metadata']['compile_environment']):
            raise MetadataError('compiler_environment_transport_mismatch')
        if compile_record['exit_code']:
            evidence['status'] = 'anonymous_compile_failed'
            return evidence, 1
        evidence['generated_object'] = _fd_digest(fds['object'])
        if evidence['generated_object']['size_bytes'] == 0:
            raise MetadataError('successful_compile_produced_no_object')
        verification_stage = 'production_version_infrastructure'
        from production_version_object import prepare_object, verify_object
        try:
            link_argv, version_evidence = prepare_object(receipt, args.receipt, args.worktree, fds,
                timeout=args.compile_timeout)
        except MetadataError as error:
            evidence['production_version_infrastructure'] = getattr(error, 'production_version_evidence', {})
            raise
        evidence['production_version_infrastructure'] = version_evidence
        link_argv += [] if symbols is None else ['-Wl,--wrap=' + value for value in symbols.values()]
        verification_stage = 'link'
        link_argv, output_transport = _stdout_link_arguments(link_argv)
        evidence['link_output_transport'] = output_transport
        if args.bug in ('C31', 'C47'):
            # Drop dependency DWARF only from the anonymous verification executable.
            link_argv.append('-Wl,--strip-debug')
            evidence['link_debug_info'] = {
                'option': '-Wl,--strip-debug',
                'adapted_argv_template_sha256': _json_digest(link_argv)}
        link_argv, archive_responses = _archive_response_arguments(link_argv, fds, receipt['files'])
        evidence['link_archive_responses'] = archive_responses
        link_record, _ = run_anonymous(link_argv, receipt['metadata']['link_cwd'], fds, timeout=args.link_timeout,
                                       stdout_fd='executable', max_scope_gib=36,
                                       environment=receipt['metadata']['link_environment'])
        evidence['link'] = link_record
        for response in archive_responses['responses']:
            if _fd_digest(fds[response['descriptor']]) != response['content']:
                raise MetadataError('anonymous_archive_response_changed_during_link')
        if link_record.get('environment_sha256') != _json_digest(receipt['metadata']['link_environment']):
            raise MetadataError('linker_environment_transport_mismatch')
        if link_record['exit_code']:
            evidence['status'] = 'anonymous_link_failed'
            return evidence, 1
        verify_object(evidence['production_version_infrastructure'], fds)
        evidence['native_executable_transport'] = _seal_native_executable(fds)
        evidence['generated_executable'] = _fd_digest(fds['executable'])
        if evidence['generated_executable']['size_bytes'] == 0:
            raise MetadataError('successful_link_produced_no_executable')
        verification_stage = 'native'
        native, stdout = run_anonymous(['@FD:executable@'], args.worktree, fds,
                                       timeout=args.native_timeout, max_scope_gib=12)
        evidence['native'] = native
        if native['watchdog_stopped'] or native['stdout_overflow'] or not native['cleanup_complete']:
            evidence['status'] = 'anonymous_native_guard_failed'
            return evidence, 1
        evidence['cases'] = parse_verdicts(stdout, count, native['exit_code'], args.bug)
        evidence['status'] = ('direct_local_contracts_passed' if all(value['passed'] for value in evidence['cases'])
                              else 'direct_local_contracts_failed')
        # Recheck hashes as well as source revision after compilation/execution.
        verification_stage = 'final_receipt'
        verify_object(evidence['production_version_infrastructure'], fds)
        verify_receipt(receipt, args.worktree, revision)
        verification_stage = 'final_generator'
        if digest_file(importlib.import_module(GENERATORS[args.bug]).__file__)['sha256'] != generator_hash:
            raise MetadataError('generator_changed_during_direct_verification')
        verification_stage = 'final_receipt_file'
        if args.receipt.read_bytes() != raw_receipt:
            raise MetadataError('production_closure_receipt_changed_during_verification')
        verification_stage = 'fixed_admission'
        if args.bug in ('C31', 'C47', 'Q09', 'Q11', 'Q20') and args.phase == 'fixed' and evidence['status'] == 'direct_local_contracts_passed':
            if not validate_fixed_admission(evidence, args.bug, revision):
                raise MetadataError('incomplete_fixed_correctness_cpp_admission')
        return evidence, 0 if evidence['status'] == 'direct_local_contracts_passed' else 2
    except MetadataError as exc:
        exc.verification_stage = verification_stage
        # Preserve sanitized process/hash evidence; rejected case verdicts are omitted.
        exc.stage_evidence = {key: evidence[key] for key in
                              ('compile', 'link', 'native', 'generated_object', 'generated_executable',
                               'link_archive_responses', 'link_output_transport', 'link_debug_info',
                               'production_version_infrastructure', 'native_executable_transport')
                              if key in evidence}
        raise
    finally:
        for fd in fds.values():
            os.close(fd)


PAIRED_DIRECT_BUGS = ('Q09', 'Q14', 'Q16', 'Q18', 'Q19', 'Q20', 'Q25', 'Q27', 'Q28')


def _current_pair_contract(bug):
    """Load the exact current adapter bytes, including its ordered coverage."""
    if bug not in PAIRED_DIRECT_BUGS:
        raise MetadataError('unknown_saved_direct_pair_contract')
    if bug in ('Q09', 'Q20'):
        from quality_statistics_cpp_contract import current_contract
        return current_contract(bug)
    name = GENERATORS[bug]
    path = HERE / (name + '.py')
    raw = path.read_bytes()
    module = ModuleType(name)
    module.__file__ = str(path)
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    specifications = module.specs() if bug in ('Q27', 'Q28') else None
    if bug == 'Q27':
        names = ['q27_' + spec['shape'] + '_' + str(spec['depth']) for spec in specifications]
    else:
        names = [name for name in module.cases() if name.startswith(bug.lower() + '_')]
    if (not names or any(not isinstance(name, str) or not name for name in names)
        or len(set(names)) != len(names)
        or specifications is not None and len(specifications) != len(names)):
        raise MetadataError('invalid_current_direct_pair_coverage')
    vector = {'case_names': names, 'specifications': specifications}
    return module, hashlib.sha256(raw).hexdigest(), vector


def _saved_pair_receipt(evidence, bug, revision):
    """Validate saved source binding without requiring the old checkout HEAD."""
    try:
        raw = Path(evidence['closure_receipt_path']).read_bytes()
        receipt = json.loads(raw)
        profile = _closure_profile(bug)
        target = 'yql/essentials/ast/ut' if profile == 'ast' else 'ydb/core/kqp/ut/rbo'
        build, metadata = receipt.get('build', {}), receipt.get('metadata', {})
        files = receipt.get('files')
        worktree = receipt.get('worktree')
        if (hashlib.sha256(raw).hexdigest() != evidence.get('closure_receipt_sha256')
            or receipt.get('schema_version') != CLOSURE_VERSION or receipt.get('revision') != revision
            or build.get('revision') != revision or type(build.get('exit_code')) is not int
            or build['exit_code'] != 0 or build.get('profile') != profile or build.get('target') != target
            or metadata.get('profile') != profile or metadata.get('target') != target
            or not isinstance(files, dict) or not files
            or not isinstance(worktree, str) or not Path(worktree).is_absolute()
            or not Path(worktree).is_dir()):
            raise MetadataError('saved_direct_pair_receipt_mismatch')
        for identity in files.values():
            if (not isinstance(identity, dict) or not isinstance(identity.get('sha256'), str)
                or not re.fullmatch('[0-9a-f]{64}', identity['sha256'])
                or type(identity.get('size_bytes')) is not int or identity['size_bytes'] < 0):
                raise MetadataError('saved_direct_pair_input_identity_missing')
        if build.get('build_record_version') is not None and (
            build.get('build_record_version') != 'production_cpp_build_v2'
            or build.get('revision_before') != revision or build.get('revision_after') != revision):
            raise MetadataError('saved_direct_pair_build_source_mismatch')
        # Immutable Git object lookup is sufficient here; the worktree may have
        # been reused for another independently verified branch since execution.
        actual = subprocess.check_output(['git', 'rev-parse', '--verify', revision + '^{commit}'],
                                         cwd=worktree, text=True, stderr=subprocess.DEVNULL).strip()
        if actual != revision:
            raise MetadataError('saved_direct_pair_source_revision_unavailable')
        return receipt
    except (OSError, ValueError, TypeError, KeyError, AttributeError, subprocess.SubprocessError) as exc:
        raise MetadataError('saved_direct_pair_receipt_unavailable_or_invalid') from exc


def _saved_q19_symbols(evidence, receipt):
    """Use only symbols captured from the actual inspected production object."""
    wrapping = evidence.get('wrapping')
    if not isinstance(wrapping, dict) or wrapping.get('both_caller_relocations_verified') is not True:
        raise MetadataError('missing_actual_q19_caller_relocation_proof')
    symbols, hashes = wrapping.get('request_symbols'), wrapping.get('request_symbol_sha256')
    if (not isinstance(symbols, dict) or set(symbols) != {'single', 'tuple'}
        or not isinstance(hashes, dict) or set(hashes) != set(symbols)):
        raise MetadataError('saved_q19_actual_wrapper_symbols_missing')
    for key, symbol in symbols.items():
        if (not isinstance(symbol, str) or not re.fullmatch(r'_Z[A-Za-z0-9_]+', symbol)
            or 'AddStatRequest' not in symbol
            or ('TMultiColumnStatistics' not in symbol if key == 'tuple' else
                'TColumnStatistics' not in symbol or 'TMultiColumnStatistics' in symbol)
            or hashes[key] != hashlib.sha256(symbol.encode()).hexdigest()):
            raise MetadataError('saved_q19_wrapper_symbol_identity_mismatch')
    for stem, key in (('kqp_column_statistics_utils.cpp', 'definition_object_sha256'),
                      ('kqp_rbo_transformer.cpp', 'caller_object_sha256')):
        path = _q19_object_path(receipt, stem)
        if wrapping.get(key) != receipt['files'][path]['sha256']:
            raise MetadataError('saved_q19_inspection_object_receipt_mismatch')
    for key in ('nm', 'relocations'):
        process = wrapping.get(key, {})
        if (type(process.get('exit_code')) is not int or process['exit_code'] != 0
            or process.get('cleanup_complete') is not True or process.get('watchdog_stopped') is not False
            or process.get('stdout_overflow') is not False):
            raise MetadataError('saved_q19_inspection_did_not_complete')
    return symbols


def _validate_saved_pair_contract(evidence, bug, revision, module, generator_hash, vector):
    if (evidence.get('generator_sha256') != generator_hash
        or type(evidence.get('case_count')) is not int
        or evidence['case_count'] != len(vector['case_names'])
        or evidence.get('direct_case_vector_sha256') != _json_digest(vector)):
        raise MetadataError('saved_direct_pair_adapter_or_coverage_changed')
    receipt = _saved_pair_receipt(evidence, bug, revision)
    if bug == 'Q27':
        source = module.render_cpp_suite(Path(receipt['worktree']), revision)
    elif bug == 'Q19':
        symbols = _saved_q19_symbols(evidence, receipt)
        source = module.render_cpp_suite(bug, symbols)
    elif bug in ('Q25', 'Q28'):
        source = module.render_cpp_suite()
    else:
        source = module.render_cpp_suite(bug)
    source = source.encode()
    if (evidence.get('generated_source_sha256') != hashlib.sha256(source).hexdigest()
        or type(evidence.get('generated_source_bytes')) is not int
        or evidence['generated_source_bytes'] != len(source)):
        raise MetadataError('saved_direct_pair_rendered_adapter_changed')


def _validate_saved_pair_process(process, phase):
    if (not isinstance(process, dict) or type(process.get('exit_code')) is not int
        or process.get('watchdog_stopped') is not False
        or process.get('cleanup_complete') is not True
        or process.get('stdout_overflow') is not False
        or process['exit_code'] not in ((0, 1) if phase == 'native' else (0,))):
        raise MetadataError('incomplete_direct_pair_process_evidence')


def pair_direct(bug, before, after, case):
    if bug in ('C31', 'C47', 'Q01', 'Q11', 'Q15'):
        raise MetadataError('fixed_admission_requires_separately_paired_sql')
    current_case = json.loads((HERE / 'cases' / (bug + '.json')).read_bytes())
    if any(case.get(key) != current_case.get(key) for key in ('base', 'commit')):
        raise MetadataError('saved_direct_pair_case_revision_changed')
    module, generator_hash, vector = _current_pair_contract(bug)
    def valid_digest(value):
        return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None
    for evidence, phase, revision in ((before, 'baseline', case['base']), (after, 'fixed', case['commit'])):
        if (evidence.get('schema_version') != 'anonymous_direct_cpp_v1' or evidence.get('bug') != bug
                or evidence.get('phase') != phase or evidence.get('revision') != revision
                or evidence.get('closure_provenance_verified') is not True
                or not evidence.get('cases') or evidence.get('generated_files_persisted') is not False
                or evidence.get('query_semantics_verified') is not False
                or evidence.get('quality_improvement_verified') is not False):
            raise MetadataError('unmatched_direct_pair_provenance')
        if (evidence.get('generator_module') != GENERATORS[bug]
                or type(evidence.get('case_count')) is not int or evidence['case_count'] <= 0
                or evidence.get('proof_scope') != ('source_extracted_helper' if bug == 'Q27' else 'linked_production_local_contracts')
                or any(not valid_digest(evidence.get(key)) for key in
                       ('generator_sha256', 'generated_source_sha256', 'closure_receipt_sha256'))):
            raise MetadataError('missing_direct_pair_identity')
        for key in ('generated_object', 'generated_executable'):
            if (not valid_digest(evidence.get(key, {}).get('sha256'))
                    or type(evidence.get(key, {}).get('size_bytes')) is not int
                    or evidence[key]['size_bytes'] <= 0):
                raise MetadataError('missing_direct_pair_binary_identity')
        for phase_name in ('compile', 'link', 'native'):
            _validate_saved_pair_process(evidence.get(phase_name), phase_name)
        _validate_saved_pair_contract(evidence, bug, revision, module, generator_hash, vector)
    if before.get('generator_sha256') != after.get('generator_sha256') or before.get('case_count') != after.get('case_count'):
        raise MetadataError('direct_pair_generator_or_case_mismatch')
    if bug != 'Q27' and before.get('generated_source_sha256') != after.get('generated_source_sha256'):
        raise MetadataError('direct_pair_adapter_source_changed')
    count = after['case_count']
    # Re-validate closed case records and exit consistency after loading evidence.
    records = []
    for evidence in (before, after):
        records.append(parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in evidence['cases']),
                                      count, evidence['native']['exit_code'], bug))
    fixed_passes = all(value['passed'] for value in records[1])
    gains = sum(not left['passed'] and right['passed'] for left, right in zip(*records))
    regressions = sum(left['passed'] and not right['passed'] for left, right in zip(*records))
    if bug in ('Q09', 'Q20'):
        if any(spec['control'] and not left['passed'] for spec, left in zip(vector['specifications'], records[0])):
            raise MetadataError('baseline_statistics_control_failed')
    if bug == 'Q27':
        if not all(value['passed'] for value in records[0]):
            raise MetadataError('baseline_collector_root_semantics_failed')
        gains = sum(right['entries'] < left['entries'] for left, right in zip(*records))
        regressions += sum(right['entries'] > left['entries'] or right['unique_visible_nodes'] != left['unique_visible_nodes']
                           for left, right in zip(*records))
    if bug == 'Q28':
        specifications = module.specs()
        if len(specifications) != count:
            raise MetadataError('scheduler_specifications_changed')
        gains = 0
        for spec, left, right in zip(specifications, *records):
            excluded = spec['unknown_control'] or spec['mode'].endswith('_exclusion') or spec['mode'] == 'raw_shared_slots'
            counters = ('output_refreshes', 'rule_attempts', 'candidate_visits')
            if any(left[key] <= 0 or right[key] <= 0 for key in counters):
                raise MetadataError('scheduler_work_measurement_is_empty')
            if excluded:
                regressions += any(right[key] != left[key] for key in counters)
            else:
                gains += all(right[key] < left[key] for key in counters)
                regressions += any(right[key] > left[key] for key in counters)
    if digest_file(module.__file__)['sha256'] != generator_hash:
        raise MetadataError('direct_pair_generator_changed_during_admission')
    local = fixed_passes and gains > 0 and regressions == 0
    return {'bug': bug, 'baseline_revision': case['base'], 'fixed_revision': case['commit'],
            'status': 'paired_local_contracts_passed_query_semantics_pending' if local else 'paired_local_contracts_pending',
            'fixed_local_contracts_passed': fixed_passes, 'improved_case_count': gains,
            'regressed_case_count': regressions, 'local_contract_improvement_verified': local,
            'query_semantics_verified': False, 'quality_finding_fully_verified': False,
            'baseline_evidence_sha256': _json_digest(before), 'fixed_evidence_sha256': _json_digest(after),
            'proof_scope': after['proof_scope']}


def validate_fixed_admission(evidence, bug, revision):
    """Validate fixed local contracts; query semantics require separate evidence."""
    if bug in ('C31', 'C47'):
        from correctness_cpp_contract import validate_fixed_local_contracts
        return validate_fixed_local_contracts(evidence, bug, revision)
    if bug in ('Q09', 'Q20'):
        from quality_statistics_cpp_contract import validate_fixed
        return validate_fixed(evidence, bug, revision)
    if bug == 'Q11':
        from quality_union_cpp_contract import validate_fixed_local_contracts
        return validate_fixed_local_contracts(evidence, revision)
    if bug == 'Q01':
        from quality_range_runtime_contract import validate_fixed_runtime
        return validate_fixed_runtime(evidence, revision)
    if bug != 'Q15' or not isinstance(evidence, dict):
        return False
    try:
        if (evidence.get('schema_version') != 'anonymous_direct_cpp_v1'
                or evidence.get('bug') != bug or evidence.get('phase') != 'fixed'
                or evidence.get('revision') != revision
                or evidence.get('status') != 'direct_local_contracts_passed'
                or evidence.get('closure_provenance_verified') is not True
                or evidence.get('generated_files_persisted') is not False
                or evidence.get('proof_scope') != 'linked_production_local_contracts'
                or evidence.get('generator_module') != GENERATORS[bug]):
            return False
        module = importlib.import_module(GENERATORS[bug])
        expected_count = len(module.cases())
        if (evidence.get('case_count') != expected_count
                or evidence.get('generator_sha256') != digest_file(module.__file__)['sha256']
                or evidence.get('generated_source_sha256') != hashlib.sha256(module.render_cpp_suite().encode()).hexdigest()):
            return False
        for key in ('closure_receipt_sha256', 'generated_source_sha256', 'generator_sha256'):
            if not isinstance(evidence.get(key), str) or not re.fullmatch('[0-9a-f]{64}', evidence[key]):
                return False
        receipt_path = Path(evidence['closure_receipt_path'])
        raw_receipt = receipt_path.read_bytes()
        receipt = json.loads(raw_receipt)
        if (hashlib.sha256(raw_receipt).hexdigest() != evidence['closure_receipt_sha256']
                or receipt.get('schema_version') != CLOSURE_VERSION
                or receipt.get('revision') != revision or receipt.get('build', {}).get('exit_code') != 0
                or receipt.get('build', {}).get('revision') != revision
                or receipt.get('build', {}).get('target') != 'ydb/core/kqp/ut/rbo'
                or receipt.get('metadata', {}).get('profile') != 'rbo' or not receipt.get('files')):
            return False
        for key in ('generated_object', 'generated_executable'):
            item = evidence.get(key, {})
            if (not re.fullmatch('[0-9a-f]{64}', item.get('sha256', ''))
                    or type(item.get('size_bytes')) is not int or item['size_bytes'] <= 0):
                return False
        for phase in ('compile', 'link', 'native'):
            process = evidence.get(phase, {})
            if (process.get('exit_code') != 0 or process.get('cleanup_complete') is not True
                    or process.get('watchdog_stopped') or process.get('stdout_overflow')):
                return False
        records = parse_verdicts(b'\n'.join(json.dumps(value).encode() for value in evidence['cases']), expected_count, 0, bug)
        return all(record['passed'] for record in records)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, MetadataError):
        return False


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    for action in ('prepare', 'export-graph', 'capture', 'run', 'measure-compilation'):
        child = sub.add_parser(action)
        child.add_argument('--worktree', type=Path, required=True)
        child.add_argument('--bug', choices=sorted(GENERATORS), required=True)
        child.add_argument('--phase', choices=('baseline', 'fixed'), required=True)
        if action in ('prepare', 'export-graph', 'capture'):
            child.add_argument('--resource', action='append', default=[])
            if action in ('export-graph', 'capture'):
                child.add_argument('--build-record', type=Path, required=True)
        else:
            child.add_argument('--receipt', type=Path, required=True)
            child.add_argument('--compile-timeout', type=int, default=900)
            child.add_argument('--link-timeout', type=int, default=900)
            child.add_argument('--native-timeout', type=int, default=120)
            if action == 'measure-compilation':
                child.add_argument('--sample-index', type=int, default=0)
    pair = sub.add_parser('pair')
    pair.add_argument('--bug', choices=sorted(GENERATORS), required=True)
    pair.add_argument('--baseline', type=Path, required=True)
    pair.add_argument('--fixed', type=Path, required=True)
    args = parser.parse_args()
    stamp = str(time.time_ns())
    try:
        if args.command == 'pair':
            case = json.loads((HERE / 'cases' / (args.bug + '.json')).read_text())
            evidence = pair_direct(args.bug, json.loads(args.baseline.read_bytes()), json.loads(args.fixed.read_bytes()), case)
            code = 0 if evidence['local_contract_improvement_verified'] else 2
        else:
            with serial_slot() as state:
                if args.command == 'prepare':
                    evidence, code = prepare(args, state)
                elif args.command == 'export-graph':
                    evidence, code = export_graph(args, args.build_record)
                elif args.command == 'capture':
                    evidence, code = capture_closure(args, args.build_record)
                elif args.command == 'measure-compilation':
                    from q28_compilation_measurement import execute_measurements
                    evidence, code = execute_measurements(args)
                else:
                    evidence, code = execute(args)
        path = HERE / 'verification' / ('direct-' + args.command + '-' + args.bug + '-' + stamp + '.json')
        _write_json(path, evidence)
        print(json.dumps({'status': evidence['status'], 'evidence': str(path)}, sort_keys=True))
        return code
    except (MetadataError, OSError, ValueError, subprocess.SubprocessError) as exc:
        reason = str(exc) if isinstance(exc, MetadataError) else type(exc).__name__
        path = HERE / 'verification' / ('direct-harness-failure-' + args.bug + '-' + stamp + '.json')
        failure = {'status': 'direct_harness_failed', 'reason': reason,
                   'command': args.command, 'bug': args.bug, 'no_runtime_verdict_accepted': True}
        if isinstance(exc, MetadataError) and hasattr(exc, 'verification_stage'):
            failure['verification_stage'] = exc.verification_stage
            failure['stage_evidence'] = getattr(exc, 'stage_evidence', {})
        _write_json(path, failure)
        print(json.dumps({'status': 'direct_harness_failed', 'reason': reason, 'evidence': str(path)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
