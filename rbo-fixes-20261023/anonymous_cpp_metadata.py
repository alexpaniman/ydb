"""Extract production compiler/link closure metadata from a successful ya graph.

Only existing production build outputs are persisted. Generated verification
sources, objects, executables and diagnostic text never enter this module.
Unknown command wrappers or unresolved resources fail closed for manual audit.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
VERSION = 'production_cpp_closure_v1'
PROFILES = {
    'kqp_runtime': {'target': 'ydb/core/kqp/runtime/streaming/ut', 'sample': 'ydb/core/kqp/runtime/streaming/kqp_streaming_aggregation_ut.cpp'},
    'ast': {'target': 'yql/essentials/ast/ut', 'sample': 'yql/essentials/ast/ut/yql_expr_ut.cpp'},
    'rbo': {'target': 'ydb/core/kqp/ut/rbo', 'sample': 'ydb/core/kqp/ut/rbo/kqp_rbo_global_ut.cpp'},
}
EXPORT_SUFFIXES = ('.a', '.o', '.h', '.hpp', '.inc', '.so', '.cpp', '.c', '.ld', '.supp', '.pyplugin')


class MetadataError(Exception):
    pass


def digest_file(path):
    path = Path(path).resolve(strict=True)
    h = hashlib.sha256()
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
        after = os.fstat(stream.fileno())
    identity = lambda stat: (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
    if identity(before) != identity(after) or identity(path.stat()) != identity(after):
        raise MetadataError('production_input_changed_during_fingerprint')
    return {'resolved_path': str(path), 'sha256': h.hexdigest(), 'size_bytes': after.st_size,
            'mtime_ns': after.st_mtime_ns, 'ctime_ns': after.st_ctime_ns,
            'device': after.st_dev, 'inode': after.st_ino}


def source_revision(worktree):
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=worktree):
        raise MetadataError('tracked_sources_are_modified')
    return subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=worktree, text=True).strip()


def production_build_arguments(profile, export_root, graph_path, *, export_objects=True):
    if profile not in PROFILES:
        raise MetadataError('unknown_production_closure_profile')
    # Output paths must be new invocation-specific directories; --output exports
    # dependency results while retaining their original relative build paths.
    # C31/C47 link production archives and drop the unit-test target's objects.
    # Missing standalone inputs still reject during metadata capture.
    suffixes = [suffix for suffix in EXPORT_SUFFIXES if export_objects or suffix != '.o']
    return [PROFILES[profile]['target'], '--no-src-links', '--output=' + str(export_root),
            '--all-outputs-to-result',
            *['--add-result=' + suffix for suffix in suffixes]]


def production_graph_arguments(build_arguments, graph_path):
    # A successful graph-only command is not a successful production build.
    return list(build_arguments) + ['--dump-json-graph', '--dump-graph-to-file=' + str(graph_path)]


def production_configuration(arguments, workers):
    # Bind the explicit guard configuration; do not claim an ambient-config snapshot.
    return {'build_mode': 'relwithdebinfo', 'workers': workers,
            'arguments': list(arguments),
            'build_guard_sha256': digest_file(HERE / 'memory_safe_build.py')['sha256']}


def validate_graph_export(build, graph_path, export_root, profile, revision):
    graph_path = Path(graph_path).resolve()
    build_path = graph_path.parent / 'build.json'
    record_path = graph_path.parent / 'graph-export.json'
    record = json.loads(record_path.read_bytes())
    configuration = build.get('configuration', {})
    workers = configuration.get('workers')
    if type(build.get('broad_object_export')) is not bool:
        raise MetadataError('graph_export_requires_recorded_object_export_policy')
    expected_arguments = production_build_arguments(profile, export_root, graph_path,
                                                   export_objects=build['broad_object_export'])
    if (build.get('build_record_version') != 'production_cpp_build_v2'
        or build.get('revision_before') != revision or build.get('revision_after') != revision
        or build.get('memory', {}).get('watchdog_stopped') is not False
        or workers != 8 or configuration != production_configuration(expected_arguments, workers)
        or record.get('schema_version') != 'production_graph_export_v1'
        or record.get('exit_code') != 0 or record.get('memory', {}).get('watchdog_stopped') is not False
        or record.get('revision_before') != revision or record.get('revision_after') != revision
        or record.get('profile') != profile or record.get('graph_path') != str(graph_path)
        or json.loads(build_path.read_bytes()) != build
        or record.get('build_record_sha256') != digest_file(build_path)['sha256']
        or record.get('configuration') != configuration
        or record.get('arguments') != production_graph_arguments(expected_arguments, graph_path)
        or record.get('graph') != digest_file(graph_path)):
        raise MetadataError('graph_export_does_not_match_successful_build')
    return record, digest_file(record_path), digest_file(build_path)


def _replace(text, substitutions):
    for key, value in substitutions.items():
        text = text.replace('$(' + key + ')', str(value))
    if '$(' in text or '${' in text:
        raise MetadataError('unresolved_build_graph_resource')
    return text


def resource_substitutions(graph, explicit):
    """Resolve only unambiguous installed sandbox resources; never download."""
    result = dict(explicit or {})
    configuration = graph.get('conf', {}) if isinstance(graph, dict) else {}
    for resource in configuration.get('resources', []):
        if not isinstance(resource, dict):
            continue
        pattern, uri = resource.get('pattern'), resource.get('resource')
        if not isinstance(pattern, str) or pattern in result:
            continue
        if uri is None and isinstance(resource.get('resources'), list):
            platform = configuration.get('platform')
            if not isinstance(platform, str):
                raise MetadataError('resource_variant_requires_graph_platform')
            variants = [variant for variant in resource['resources']
                        if isinstance(variant, dict) and isinstance(variant.get('platform'), str)
                        and variant['platform'].casefold() == platform.casefold()]
            if len(variants) != 1:
                raise MetadataError('graph_platform_resource_is_ambiguous')
            uri = variants[0].get('resource')
        if not isinstance(uri, str):
            continue
        matched = re.fullmatch(r'sbr:(\d+)', uri)
        if matched:
            candidate = Path.home() / '.ya/tools/v4' / matched.group(1)
            if candidate.is_dir():
                result[pattern] = str(candidate.resolve())
    return result



def audited_command_environment(raw, substitutions, platform):
    if not raw:
        return {}
    names = {'ARCADIA_ROOT_DISTBUILD', 'CPATH', 'DYLD_LIBRARY_PATH', 'LIBRARY_PATH', 'SDKROOT'}
    if (not isinstance(raw, dict) or set(raw) != names
        or not isinstance(platform, str) or platform.casefold() != 'linux'
        or any(not isinstance(value, str) for value in raw.values())
        or any(raw[name] != '' for name in ('CPATH', 'LIBRARY_PATH', 'SDKROOT'))
        or raw['ARCADIA_ROOT_DISTBUILD'] != '$(SOURCE_ROOT)'
        or not re.fullmatch(r'\$\(CLANG-[0-9]+\)/lib:\$\(OS_SDK_ROOT-sbr:[0-9]+\)/usr/lib/x86_64-linux-gnu',
                            raw['DYLD_LIBRARY_PATH'])):
        raise MetadataError('selected_command_environment_requires_audit')
    return {name: _replace(value, substitutions) for name, value in raw.items()}


def require_non_input_tool_root(raw_arguments, raw_cwd, raw_environment):
    # The installed compiler uses TOOL_ROOT only to normalize diagnostic/macro
    # file names. A new input-path use must be audited before omitting its tree.
    token = '$(TOOL_ROOT)'
    allowed = {'-fdebug-prefix-map=$(TOOL_ROOT)=/-T', '-fmacro-prefix-map=$(TOOL_ROOT)/='}
    # Response files could hide another input-path use from this raw audit.
    if (any(value.startswith('@') for value in raw_arguments)
        or any(token in value and value not in allowed for value in raw_arguments)
        or token in (raw_cwd or '')
        or any(token in value for value in (raw_environment or {}).values())):
        raise MetadataError('tool_root_reference_requires_input_audit')


def _flatten(arguments, cwd):
    result = []
    for arg in arguments:
        if arg in ('--ya-start-command-file', '--ya-end-command-file'):
            continue
        if arg.startswith('@'):
            path = Path(arg[1:])
            if not path.is_absolute():
                path = Path(cwd) / path
            # Ya's response-file protocol is exactly one argument per line.
            if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
                raise MetadataError('response_file_unavailable_or_oversized')
            result.extend(_flatten(path.read_text().splitlines(), path.parent))
        else:
            result.append(arg)
    return result


def _compiler_index(arguments):
    indexes = [i for i, arg in enumerate(arguments)
               if Path(arg).name in ('clang++', 'clang', 'clang-18', 'clang++-18')]
    if len(indexes) != 1:
        raise MetadataError('production_clang_driver_is_ambiguous')
    return indexes[0]


def _unwrap(arguments, worktree, cwd, linking):
    arguments = _flatten(arguments, cwd)
    compiler = _compiler_index(arguments)
    prefix = arguments[:compiler]
    if linking:
        script_positions = [i for i, arg in enumerate(prefix) if arg.endswith('/build/scripts/link_exe.py')]
        if script_positions:
            if len(script_positions) != 1:
                raise MetadataError('ambiguous_link_wrapper')
            script = Path(prefix[script_positions[0]])
            if script.resolve() != (Path(worktree) / 'build/scripts/link_exe.py').resolve():
                raise MetadataError('link_wrapper_from_other_source_tree')
            wrapper_args = arguments[script_positions[0] + 1:]
            if '--start-plugins' in wrapper_args:
                begin = wrapper_args.index('--start-plugins')
                end = wrapper_args.index('--end-plugins')
                if end != begin + 1:
                    raise MetadataError('nonempty_link_plugin_requires_audited_adapter')
                wrapper_args = wrapper_args[:begin] + wrapper_args[end + 1:]
            if '--custom-step' in wrapper_args:
                raise MetadataError('link_plugin_or_custom_step_requires_audited_adapter')
            scripts = str(script.parent)
            original_sys_path = list(sys.path)
            sys.path.insert(0, scripts)
            original_bytecode = sys.dont_write_bytecode
            sys.dont_write_bytecode = True
            dependency_names = ('process_command_files', 'thinlto_cache', 'process_whole_archive_option')
            previous_modules = {name: sys.modules.pop(name, None) for name in dependency_names}
            try:
                # Reuse the exact revision's option and whole-archive handling.
                module_spec = importlib.util.spec_from_file_location('_rbo_production_link_exe', script)
                module = importlib.util.module_from_spec(module_spec)
                module_spec.loader.exec_module(module)
                opts, args = module.parse_args(wrapper_args)
                if opts.thinlto_cache or getattr(opts, 'thinlto_cache_dir', None):
                    raise MetadataError('thinlto_cache_requires_separate_audit')
                args = module.remove_excessive_flags(module.pcf.skip_markers(args))
                if opts.exclude_libs:
                    args = module.remove_libs(args, opts.exclude_libs)
                args = module.ProcessWholeArchiveOption(opts.arch, opts.whole_archive_peers,
                                                       opts.whole_archive_libs).construct_cmd(args)
                return args
            finally:
                sys.path[:] = original_sys_path
                sys.dont_write_bytecode = original_bytecode
                for name, previous in previous_modules.items():
                    sys.modules.pop(name, None)
                    if previous is not None:
                        sys.modules[name] = previous
        if prefix:
            raise MetadataError('unknown_production_link_wrapper')
    elif prefix:
        # Python command wrappers may preprocess flags or invoke tidy. Silently
        # skipping them would give a different ABI/command than the build.
        raise MetadataError('unknown_production_compile_wrapper')
    return arguments[compiler:]


LINKER_INPUT_OPTIONS = ('--version-script', '--dynamic-list')


def _canonical_linker_arguments(arguments):
    """Keep driver ordering while making each forwarded linker token explicit."""
    result = []
    iterator = iter(arguments)
    for argument in iterator:
        if argument == '-Xlinker':
            argument = next(iterator, None)
            if not argument or ',' in argument:
                raise MetadataError('unsupported_forwarded_linker_argument')
            result.append('-Wl,' + argument)
        elif argument.startswith('-Wl,'):
            parts = argument[4:].split(',')
            if any(not part or part.startswith('@') for part in parts):
                raise MetadataError('unsupported_forwarded_linker_argument')
            result.extend('-Wl,' + part for part in parts)
        else:
            result.append(argument)
    return result


def _linker_input_paths(arguments):
    pending = False
    for argument in _canonical_linker_arguments(arguments):
        if pending:
            if not argument.startswith('-Wl,/'):
                raise MetadataError('unbound_linker_auxiliary_input')
            yield argument[4:]
            pending = False
        elif argument.startswith('-Wl,'):
            token = argument[4:]
            if token in LINKER_INPUT_OPTIONS:
                pending = True
            elif token.startswith(tuple(option + '=' for option in LINKER_INPUT_OPTIONS)):
                value = token.split('=', 1)[1]
                if not value.startswith('/'):
                    raise MetadataError('unbound_linker_auxiliary_input')
                yield value
    if pending:
        raise MetadataError('missing_linker_auxiliary_input')


def _normalize_paths(arguments, cwd):
    """Ya link nodes use build-root-relative object/archive paths."""
    result = []
    path_next = False
    linker_path_next = False
    for argument in _canonical_linker_arguments(arguments):
        if linker_path_next:
            if not argument.startswith('-Wl,') or argument[4:].startswith(('-', '@')):
                raise MetadataError('missing_linker_auxiliary_input')
            value = argument[4:]
            result.append('-Wl,' + str((Path(cwd) / value).absolute()))
            linker_path_next = False
            continue
        if path_next:
            result.append(str((Path(cwd) / argument).absolute()) if not Path(argument).is_absolute() else argument)
            path_next = False
            continue
        if argument in ('-I', '-isystem', '-iquote', '-L', '-B', '-include', '-imacros', '-o', '-MF', '-MT', '-MQ'):
            result.append(argument)
            path_next = True
        elif not argument.startswith('-') and not Path(argument).is_absolute() and argument.endswith(
                ('.o', '.a', '.so', '.cpp', '.cc', '.c', '.h', '.inc', '.ld', '.supp')):
            result.append(str((Path(cwd) / argument).absolute()))
        elif argument.startswith(('-I', '-L', '-B')) and len(argument) > 2 and not Path(argument[2:]).is_absolute():
            result.append(argument[:2] + str((Path(cwd) / argument[2:]).absolute()))
        elif argument in tuple('-Wl,' + option for option in LINKER_INPUT_OPTIONS):
            result.append(argument)
            linker_path_next = True
        elif argument.startswith(tuple('-Wl,' + option + '=' for option in LINKER_INPUT_OPTIONS)):
            prefix, value = argument.split('=', 1)
            if not value or value.startswith(('-', '@')):
                raise MetadataError('missing_linker_auxiliary_input')
            result.append(prefix + '=' + (str((Path(cwd) / value).absolute()) if not Path(value).is_absolute() else value))
        else:
            result.append(argument)
    if path_next or linker_path_next:
        raise MetadataError('missing_production_path_argument')
    return result


def _remove_output(arguments):
    result, outputs = [], []
    index = 0
    while index < len(arguments):
        arg = arguments[index]
        if arg == '-o':
            if index + 1 == len(arguments):
                raise MetadataError('missing_output_argument')
            outputs.append(arguments[index + 1])
            index += 2
            continue
        result.append(arg)
        index += 1
    if len(outputs) != 1:
        raise MetadataError('ambiguous_production_output')
    return result, outputs[0]


def _no_disk_emission(arguments):
    denied = ('-save-temps', '--save-temps', '-ftime-trace', '-fmodules', '-flto',
              '-fprofile', '-fcoverage', '-gsplit-dwarf', '-fno-integrated-as',
              '-MJ', '-serialize-diagnostics', '-foptimization-record',
              '-fdiagnostics-serialization-file', '-fcrash-diagnostics-dir', '-fproc-stat-report')
    if any(arg.startswith(denied) or any(word in arg for word in
           ('thinlto-cache', '--reproduce=', '-Map=', '--Map=', '--dependency-file='))
           for arg in arguments):
        raise MetadataError('compiler_or_linker_may_persist_generated_input')
    for argument in _canonical_linker_arguments(arguments):
        if not argument.startswith('-Wl,'):
            continue
        token = argument[4:]
        if token.startswith(('--reproduce', '--time-trace', '-Map', '--Map',
                             '--dependency-file', '--why-extract', '--print-archive-stats',
                             '--save-temps', '--thinlto-cache')):
            raise MetadataError('compiler_or_linker_may_persist_generated_input')
        if token.startswith(('@', '--script', '-T', '--default-script', '-dT',
                             '--retain-symbols-file', '--symbol-ordering-file', '--just-symbols')):
            raise MetadataError('linker_auxiliary_input_requires_audited_adapter')


def compile_template(arguments, source):
    _no_disk_emission(arguments)
    arguments, _ = _remove_output(arguments)
    output = []
    skip = False
    removed = 0
    for arg in arguments:
        if skip:
            skip = False
            continue
        if arg in ('-MF', '-MT', '-MQ'):
            skip = True
        elif arg in ('-MD', '-MMD', '-MP', '-c') or arg.startswith('-g') or arg.startswith('-std='):
            continue
        elif arg == source:
            removed += 1
        else:
            output.append(arg)
    if removed != 1 or skip:
        raise MetadataError('production_source_argument_is_ambiguous')
    # Integrated assembly and -o - avoid Clang's atomic named-output files.
    return output + ['-std=c++20', '-g0', '-fno-crash-diagnostics', '-fintegrated-as',
                     '-x', 'c++', '-c', '@FD:source@', '-o', '-']


def link_template(arguments, target, export_root):
    _no_disk_emission(arguments)
    arguments, _ = _remove_output(arguments)
    root = Path(export_root).resolve()
    keep, removed = [], []
    for arg in arguments:
        path = Path(arg)
        relative = None
        if path.is_absolute():
            try:
                relative = str(path.relative_to(root))
            except ValueError:
                pass
        if relative and (relative.startswith(target.rstrip('/') + '/')
                         or relative.startswith('library/cpp/testing/unittest_main/')):
            if not relative.endswith(('.o', '.a')):
                raise MetadataError('unknown_unittest_link_input_kind')
            removed.append(relative)
            continue
        keep.append(arg)
    if not removed or not any(name.startswith('library/cpp/testing/unittest_main/') for name in removed):
        raise MetadataError('unittest_main_was_not_identified')
    # LLD's default atomic output creates a named temporary. This option uses
    # its in-memory output buffer and writes directly to the inherited memfd.
    keep += ['@FD:object@', '-Wl,--no-mmap-output-file', '-o', '@FD:executable@']
    return keep, removed


def extract_metadata(graph, worktree, export_root, profile, resources=None):
    """Pure graph adaptation apart from reading response files/production scripts."""
    worktree, export_root = Path(worktree).resolve(), Path(export_root).resolve()
    definition = PROFILES[profile]
    resources = resource_substitutions(graph, resources)
    substitutions = dict(resources, SOURCE_ROOT=str(worktree), BUILD_ROOT=str(export_root))
    nodes = graph.get('graph', graph.get('nodes')) if isinstance(graph, dict) else graph
    if not isinstance(nodes, list):
        raise MetadataError('unsupported_ya_graph_schema')
    compiles, links = [], []
    generated = set()
    outputs_by_node = {}
    source = str(worktree / definition['sample'])
    for node in nodes:
        outputs = [_replace(value, substitutions) for value in node.get('outputs', [])]
        for output in outputs:
            if output.endswith(EXPORT_SUFFIXES):
                generated.add(output)
        outputs_by_node[node.get('uid', '')] = outputs
        for command in node.get('cmds', []):
            raw = command.get('cmd_args', command.get('args'))
            if not isinstance(raw, list):
                continue
            # Expand resource tokens only for selected commands. Unrelated host
            # build nodes can legitimately have other toolchain resources.
            selected_compile = any(value.endswith('/' + definition['sample']) for value in raw)
            selected_link = any('/' + definition['target'] + '/' in output for output in outputs) and any(
                str(value).endswith('/build/scripts/link_exe.py') for value in raw)
            if not selected_compile and not selected_link:
                continue
            environment = audited_command_environment(command.get('env'), substitutions,
                                                      graph.get('conf', {}).get('platform'))
            require_non_input_tool_root(raw, command.get('cwd'), command.get('env'))
            args = [_replace(value, substitutions) for value in raw]
            cwd = _replace(command.get('cwd') or '$(BUILD_ROOT)', substitutions)
            if selected_compile:
                compiles.append((_normalize_paths(_unwrap(args, worktree, cwd, False), cwd), node.get('uid'), cwd, environment))
            if selected_link:
                links.append((_normalize_paths(_unwrap(args, worktree, cwd, True), cwd), node.get('uid'), cwd, environment))
    if len(compiles) != 1 or len(links) != 1:
        raise MetadataError('selected_production_commands_are_ambiguous')
    compile_args = compile_template(compiles[0][0], source)
    link_args, removed = link_template(links[0][0], definition['target'], export_root)
    files = set()
    for path in _linker_input_paths(compile_args + link_args):
        if not Path(path).is_file():
            raise MetadataError('selected_linker_auxiliary_input_is_missing')
        files.add(path)
    for arg in compile_args + link_args:
        if arg.startswith('/') and Path(arg).is_file():
            files.add(arg)
        elif arg.startswith('/') and arg.endswith(('.a', '.o', '.so', '.ld')):
            raise MetadataError('selected_link_input_is_missing')
        elif arg.startswith('-Wl,'):
            files.update(part for part in arg.split(',') if part.startswith('/') and Path(part).is_file())
        elif '=' in arg:
            value = arg.split('=', 1)[1]
            if value.startswith('/') and Path(value).is_file():
                files.add(value)
    # Bind the actual linker and installed resource headers/libraries as well
    # as the compiler. An unchanged clang executable alone cannot prove an
    # unchanged resource include tree or C++ runtime archive.
    compiler = Path(compile_args[0]).resolve()
    linker = compiler.parent / 'ld.lld'
    linker_flavors = [arg.split('=', 1)[1] for arg in link_args if arg.startswith('-fuse-ld=')]
    linker_paths = [arg.split('=', 1)[1] for arg in link_args if arg.startswith('--ld-path=')]
    # Linux production flags specify both the LLD flavor and its resource path.
    if linker_flavors == ['lld'] and len(linker_paths) == 1 and Path(linker_paths[0]).is_absolute():
        requested_linkers = linker_paths
    else:
        requested_linkers = linker_flavors + linker_paths
    if len(requested_linkers) != 1 or (requested_linkers[0] != 'lld' and Path(requested_linkers[0]).name != 'ld.lld'):
        raise MetadataError('anonymous_link_requires_explicit_lld_driver_selection')
    if requested_linkers[0] != 'lld':
        linker = Path(requested_linkers[0])
    if not linker.is_file():
        raise MetadataError('selected_lld_binary_unavailable')
    files.add(str(linker))
    # TOOL_ROOT is a common cache ancestor, not a compiler input. Its only raw
    # occurrences were checked above; retain every actual tool/SDK resource.
    environment_values = list(compiles[0][3].values()) + list(links[0][3].values())
    used_resources = {value for name, value in resources.items() if name != 'TOOL_ROOT'
                      and any(str(value) in arg for arg in compile_args + link_args + environment_values)}
    for root in used_resources:
        for file in Path(root).rglob('*'):
            if file.is_file():
                files.add(str(file))
    # Bind exported generated headers and the two optional Q19 inspection
    # objects. Host tool-build objects in the same graph are not inputs to the
    # selected target link; they need not have been exported as target results.
    captured_generated = 0
    for path in generated:
        relevant = path.endswith(('.h', '.hpp', '.inc', '.ld', '.supp')) or any(
            Path(path).name in (stem + '.o', stem + '.pic.o', stem + '.abi_stable.o')
            for stem in ('kqp_column_statistics_utils.cpp', 'kqp_rbo_transformer.cpp'))
        if relevant and Path(path).is_file():
            files.add(path)
            captured_generated += 1
    return {'profile': profile, 'target': definition['target'],
            'compile_argv': compile_args, 'link_argv': link_args,
            'compile_cwd': compiles[0][2], 'link_cwd': links[0][2],
            'compile_environment': compiles[0][3], 'link_environment': links[0][3],
            'tool_root_prefix_maps_only': True,
            'compile_node_uid': compiles[0][1], 'link_node_uid': links[0][1],
            'removed_test_inputs': sorted(removed), 'files': sorted(files),
            'graph_output_count': len(generated), 'captured_generated_input_count': captured_generated,
            'resources': resources}


def issue_receipt(worktree, revision, build, graph_path, export_root, profile, resources=None):
    if build.get('exit_code') != 0 or build.get('revision') != revision:
        raise MetadataError('successful_exact_revision_closure_build_required')
    if source_revision(worktree) != revision:
        raise MetadataError('closure_source_revision_mismatch')
    if build.get('profile') != profile or str(Path(build.get('export_root', '')).resolve()) != str(Path(export_root).resolve()):
        raise MetadataError('closure_build_export_identity_mismatch')
    if build.get('target') != PROFILES[profile]['target'] or Path(build.get('graph_path', '')).resolve() != Path(graph_path).resolve():
        raise MetadataError('successful_build_target_or_graph_mismatch')
    graph_path = Path(graph_path)
    if graph_path.stat().st_size > 512 * 1024 * 1024:
        raise MetadataError('build_graph_exceeds_bounded_parser_limit')
    graph_identity = digest_file(graph_path)
    graph_export, graph_export_identity, build_identity = validate_graph_export(
        build, graph_path, export_root, profile, revision)
    metadata = extract_metadata(json.loads(graph_path.read_bytes()), worktree, export_root, profile, resources)
    identities = {path: digest_file(path) for path in metadata.pop('files')}
    if source_revision(worktree) != revision or digest_file(graph_path) != graph_identity:
        raise MetadataError('production_inputs_changed_during_receipt')
    return {'schema_version': VERSION, 'worktree': str(Path(worktree).resolve()),
            'revision': revision, 'recorded_unix_ns': time.time_ns(),
            'build': build, 'build_record': build_identity, 'graph': graph_identity,
            'graph_export': graph_export, 'graph_export_record': graph_export_identity,
            'metadata': metadata, 'files': identities}


def verify_receipt(receipt, worktree, revision):
    if receipt.get('schema_version') != VERSION or receipt.get('revision') != revision:
        raise MetadataError('invalid_closure_receipt')
    if receipt.get('worktree') != str(Path(worktree).resolve()) or source_revision(worktree) != revision:
        raise MetadataError('current_closure_source_mismatch')
    if receipt.get('build', {}).get('exit_code') != 0 or not receipt.get('files'):
        raise MetadataError('unproven_production_closure')
    for path, identity in receipt['files'].items():
        current = digest_file(path)
        # Cache hardlink churn can change ctime between completed snapshots.
        # digest_file still checks full identity, including ctime, during reads.
        if ({key: value for key, value in current.items() if key != 'ctime_ns'}
                != {key: value for key, value in identity.items() if key != 'ctime_ns'}):
            raise MetadataError('production_closure_input_changed')
    if digest_file(receipt['graph']['resolved_path']) != receipt['graph']:
        raise MetadataError('production_build_graph_changed')
    # Existing v1 receipts remain usable without fabricating historical binding.
    # Newly captured receipts must retain both immutable lifecycle records.
    if receipt['build'].get('build_record_version') is not None or 'graph_export_record' in receipt:
        if (receipt['build'].get('build_record_version') != 'production_cpp_build_v2'
            or not isinstance(receipt.get('build_record'), dict)
            or not isinstance(receipt.get('graph_export_record'), dict)):
            raise MetadataError('production_graph_lifecycle_records_missing')
        if digest_file(receipt['build_record']['resolved_path']) != receipt['build_record']:
            raise MetadataError('production_build_record_changed')
        if digest_file(receipt['graph_export_record']['resolved_path']) != receipt['graph_export_record']:
            raise MetadataError('production_graph_export_record_changed')
    return True
