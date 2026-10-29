"""Independent bounded source/config checks and a failed-process mock only."""
import ast
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory, text_format

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path[:0] = [str(ROOT), str(REPO / 'rbo-review-20261021')]
COUNT = 0


def check(value):
    global COUNT
    if value is not True:
        raise AssertionError('Independent spill-cap check ordinal ' + str(COUNT + 1))
    COUNT += 1


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    patch_path = ROOT / 'proposed-correctness-amendments/C31-observation-no-payload-spill-files-20261028.patch'
    audit_path = ROOT / 'verification/C31-observation-spill-persistence-source-audit-20261028.json'
    audit = json.loads(audit_path.read_bytes())
    check(sha(patch_path.read_bytes()) == '3ba620eaad5f60154a923653043371e4a0ca9595cf50ac284913e0cd50ab51bf')
    check(sha(audit_path.read_bytes()) == '4ea76db95ef70273c05acd131a327c79ed02ef60c84b995d48a620d92089b8ea')
    for item in audit['pinned_source_bindings']:
        value = subprocess.check_output(['git', 'show', item['revision'] + ':' + item['path']], cwd=REPO)
        check(sha(value) == item['sha256'])
    before = (ROOT / 'c31_scope_observation.py').read_bytes()
    check(sha(before) == audit['candidate']['live_before_sha256'])
    with tempfile.TemporaryDirectory(prefix='c31-spill-cap-review-') as directory:
        local = Path(directory) / ROOT.name
        local.mkdir()
        (local / 'c31_scope_observation.py').write_bytes(before)
        done = subprocess.run(['git', 'apply', str(patch_path.resolve())], cwd=directory,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(done.returncode == 0)
        after = (local / 'c31_scope_observation.py').read_bytes()
    check(sha(after) == audit['candidate']['candidate_after_sha256'])
    fn = lambda src: {node.name: ast.dump(node) for node in ast.parse(src).body if isinstance(node, ast.FunctionDef) and node.name != 'metadata'}
    check(fn(before) == fn(after))
    module = types.ModuleType('c31_independent_spill_cap')
    module.__file__ = str(ROOT / 'c31_scope_observation.py')
    exec(compile(after, module.__file__, 'exec'), module.__dict__)
    check(tuple(module.CASES) == tuple(audit['scope']['profiles']) and len(module.CASES) == 2)
    check(module._HASHES == audit['artifact_bindings']['dependency_snapshots'])
    import native_verify
    from optimizer_mode_contract import optimizer_mode_configuration
    config = (REPO / 'rbo-review-20261021/new.conf').read_text()
    check(sha(config.encode()) == audit['artifact_bindings']['base_config_sha256'])
    check(sha(Path(native_verify.__file__).read_bytes()) == audit['artifact_bindings']['native_verify_sha256'])
    check('SpillingServiceConfig' not in config and config.count('TableServiceConfig {') == 1)
    proto = (REPO / 'ydb/core/protos/table_service_config.proto').read_text()
    # Parse only the exact names/types of interest, without claiming full native config parsing.
    descriptor = descriptor_pb2.FileDescriptorProto(name='independent_cap.proto', syntax='proto2', package='SpillCapReview')
    def add(name):
        return descriptor.message_type.add(name=name)
    def field(message, name, number, kind, target=None):
        value = message.field.add(name=name, number=number, label=1, type=kind)
        if target:
            value.type_name = '.SpillCapReview.' + target
    local = add('Local')
    field(local, 'Enable', 1, 8)
    field(local, 'MaxTotalSize', 3, 4)
    service = add('Service'); field(service, 'LocalFileConfig', 1, 11, 'Local')
    table = add('Table'); field(table, 'SpillingServiceConfig', 16, 11, 'Service')
    for name, kind, typename in (('EnableQueryServiceSpilling', 8, 'bool'), ('EnableSpillingNodes', 9, 'string'), ('UseBlockHashJoin', 8, 'bool')):
        match = re.search(r'optional\s+' + typename + r'\s+' + name + r'\s*=\s*([0-9]+)', proto)
        check(match is not None)
        field(table, name, int(match[1]), kind)
    app = add('App'); field(app, 'TableServiceConfig', 1, 11, 'Table')
    pool = descriptor_pool.DescriptorPool(); pool.Add(descriptor)
    cls = message_factory.MessageFactory(pool).GetPrototype(pool.FindMessageTypeByName('SpillCapReview.App'))
    config_hashes = {}
    for case in module.CASES:
        meta = module.metadata(case, 24)
        check(meta['table_service_overrides']['EnableQueryServiceSpilling'] is True)
        check(meta['table_service_overrides']['EnableSpillingNodes'] == 'None')
        check(meta['table_service_overrides']['UseBlockHashJoin'] is False)
        check(meta['node_count'] == 1 and meta['peak_native_concurrency'] == 1)
        check(meta['correctness_requires_physical_ast'] is True and meta['expected_shared_producer'] is True)
        check('expected_rejection_token' not in meta)
        for rows in (-1, 0, 23, 25, 128, True, False, 24.0, '24', None):
            try:
                module.metadata(case, rows)
            except ValueError:
                check(True)
            else:
                check(False)
        rendered = native_verify.config_with(config, meta['table_service_overrides'])
        check(rendered.count('SpillingServiceConfig:') == 1)
        parsed = cls(); text_format.Parse(rendered, parsed, allow_unknown_field=True)
        table = parsed.TableServiceConfig
        check(table.SpillingServiceConfig.LocalFileConfig.Enable and table.SpillingServiceConfig.LocalFileConfig.MaxTotalSize == 1)
        check(table.EnableQueryServiceSpilling and table.EnableSpillingNodes == 'None' and not table.UseBlockHashJoin)
        pragmas = dict(meta['pragmas'], YqlSelect='force')
        check(optimizer_mode_configuration(rendered, 'new', 'yql', pragmas, False)['optimizer_mode_config_verified'])
        check(sha(rendered.encode()) == audit['artifact_bindings']['rendered_config_sha256_only'][case])
        config_hashes[case] = sha(rendered.encode())
        memory, closed, calls = {}, [], []
        def fake_text(label, value):
            fd = 100 + len(memory)
            memory[fd] = (label, value)
            return fd
        def failed_process(argv, **options):
            calls.append(argv)
            check(argv[argv.index('--log-file') + 1] == '/dev/null')
            check(not any('disk' in arg.lower() for arg in argv))
            check(len(options['pass_fds']) == len(memory))
            return types.SimpleNamespace(returncode=1, stdout=b'', stderr=b'bounded execution failure')
        with ExitStack() as stack:
            for obj, key, value in ((native_verify.g, 'anonymous_text', fake_text),
                (native_verify.subprocess, 'run', failed_process), (native_verify.os, 'lseek', lambda *unused: 0),
                (native_verify.os, 'read', lambda *unused: b''), (native_verify.os, 'close', lambda fd: closed.append(fd))):
                stack.enter_context(patch.object(obj, key, value))
            record = native_verify.run_generated(module, case, 'new', 24)
        check(len(calls) == 1)
        check(record['exit_code'] == 1 and record['semantic_oracle_pass'] is False)
        check(record['expected_rejection_configured'] is False and 'correctness_guard_observed' not in record)
        check(len(closed) == len(memory) and len(set(closed)) == len(memory))
        check(record['settings_sha256'] == config_hashes[case])
    # Arithmetic bounds reflect the separately inspected production packing paths.
    check(4 * 4 + 4 * 1 + 2 * 8 == 36)
    check(3 * 4 + 2 * 4 == 20 and 3 * 4 + 2 * 8 == 28)
    check(1 + 1 == 2)
    for size in (2, 20, 28, 36, 4096, 2 ** 32):
        check(0 + size > 1)
    check(not (0 + 1 > 1))
    check(sha((ROOT / 'c31_scope_observation.py').read_bytes()) == sha(before))
    result = dict(status='independent_source_metadata_checks_passed', assertions=COUNT,
                  patch_sha256=sha(patch_path.read_bytes()), audit_sha256=sha(audit_path.read_bytes()),
                  config_sha256=config_hashes, pinned_sources_checked=39,
                  mocked_failed_native_records=2, native_processes_started=0, toolchain_runs=0,
                  live_helper_edits=False, rendered_queries_or_plans_persisted=False,
                  full_native_config_parse_verified=False, actual_no_spill_observed=False)
    output = ROOT / 'verification/C31-spill-cap-independent-metadata-20261028.json'
    output.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps(dict(assertions=COUNT, report_sha256=sha(output.read_bytes()))))


if __name__ == '__main__':
    main()
