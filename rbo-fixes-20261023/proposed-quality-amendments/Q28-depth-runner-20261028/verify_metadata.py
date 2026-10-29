"""Source and fake-descriptor checks only; never invoke a compiler or native child."""
import ast
import copy
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DRIVER = HERE / 'q28_depth_verify.py'
GENERATOR = ROOT / 'proposed-quality-amendments/Q28-map-fusion-refresh-depth-20261028/quality_scheduler_depth_checks.py'
NEW_REVISION = 'a' * 40  # Fake metadata identity, never a production admission token.


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path):
    module = ModuleType(path.stem)
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def records(module):
    result = []
    for index, (spec, checks) in enumerate(zip(module.SPECS, module.EXPECTED_CHECKS)):
        row = dict(case_index=index, checks_executed=checks, passed=True)
        for name, value in (('source_refreshes', 100), ('union_refreshes', 10),
                            ('rule_attempts', 100), ('candidate_visits', 100)):
            row['current_' + name] = value
            delta = (int(spec['kept']) if name == 'union_refreshes' else
                     0 if name == 'source_refreshes' and spec['kept'] else
                     1 if spec['kept'] else spec['fanout'])
            row['old_depth_' + name] = value + delta
        result.append(row)
    return result


def encoded(value):
    return b'\n'.join(json.dumps(x, sort_keys=True).encode() for x in value)


class FakeRun:
    def __init__(self, module, fixture, *, fail=None, allocation_failure=None):
        self.module, self.fixture = module, fixture
        self.fail, self.allocation_failure = fail, allocation_failure
        self.allocations = 0
        self.opened, self.closed, self.commands = {}, [], []
        self.version_checks = 0
        self.receipt_checks = 0
        self.generator = load(GENERATOR)
        self.generator.ROOT = fixture
        self.metadata = SimpleNamespace(MetadataError=type('MetadataError', (Exception,), {}),
                                        verify_receipt=self.verify_receipt, source_revision=lambda _: NEW_REVISION)
        self.direct = SimpleNamespace(_expected_revision=lambda *args: NEW_REVISION,
            pair_direct=self.pair_direct, _memfd=self.memfd, _fd_digest=self.fd_digest,
            run_anonymous=self.run, _stdout_link_arguments=self.stdout_link,
            _archive_response_arguments=self.archive_responses, _seal_native_executable=self.seal)
        self.version = SimpleNamespace(prepare_object=self.prepare, verify_object=self.verify_version)

    def trigger(self, name):
        if self.fail == name:
            raise RuntimeError(name)
        if self.fail == name + '_cancel':
            raise KeyboardInterrupt()

    def memfd(self, name, data=b'', executable=False):
        self.allocations += 1
        if self.allocation_failure == self.allocations:
            raise OSError('fake allocation interruption')
        fd = 1000 + self.allocations
        self.opened[fd] = bytes(data)
        return fd

    def close(self, fd):
        if fd not in self.opened or fd in self.closed:
            raise AssertionError('descriptor closed without ownership')
        self.closed.append(fd)
        del self.opened[fd]

    def fd_digest(self, fd):
        data = self.opened[fd]
        return {'sha256': sha(data), 'size_bytes': len(data)}

    def verify_receipt(self, receipt, worktree, revision):
        self.receipt_checks += 1
        assert revision == NEW_REVISION
        self.trigger('initial_receipt' if self.receipt_checks == 1 else 'final_receipt')
        return True

    def pair_direct(self, bug, before, after, case):
        self.trigger('pair')
        assert bug == 'Q28' and before['case_count'] == after['case_count'] == 26
        return {'local_contract_improvement_verified': self.fail != 'unaccepted_pair',
                'query_semantics_verified': False, 'quality_finding_fully_verified': False}

    def prepare(self, receipt, path, worktree, fds, timeout):
        fds['production_version_object'] = self.memfd('version', b'production version')
        self.trigger('prepare')
        if self.fail == 'prepare_metadata_error':
            error = self.metadata.MetadataError('fake')
            error.production_version_evidence = {'failure_recorded': True}
            raise error
        return ['link', '-o', '@FD:executable@'], {'version': 'fake-bound-version'}

    def verify_version(self, evidence, fds):
        self.version_checks += 1
        self.trigger('version_verify_' + str(self.version_checks))
        assert 'production_version_object' in fds

    def stdout_link(self, arguments):
        return ['link', '-o', '-'], {'transport': 'stdout_to_executable_memfd'}

    def archive_responses(self, arguments, fds, files):
        rows = []
        for index in range(2):
            name = 'link_archives_' + str(index)
            fds[name] = self.memfd('archive', b'bound archive')
            rows.append({'descriptor': name, 'content': self.fd_digest(fds[name])})
        return arguments, {'responses': rows}

    def seal(self, fds):
        self.trigger('seal')
        old = fds['executable']
        new = self.memfd('readonly executable', self.opened[old])
        fds['executable'] = new
        self.close(old)
        self.trigger('after_seal')
        return {'version': 'sealed_read_only_memfd_v1', 'same_inode': True,
                'write_grow_shrink_and_seal_seals': True, 'writable_owner_closed': True}

    def run(self, command, cwd, fds, **kwargs):
        stage = 'compile' if kwargs.get('stdout_fd') == 'object' else 'link' if kwargs.get('stdout_fd') == 'executable' else 'native'
        self.commands.append((stage, command, copy.deepcopy(kwargs)))
        self.trigger(stage)
        record = dict(exit_code=0, cleanup_complete=True, watchdog_stopped=False, stdout_overflow=False)
        if 'environment' in kwargs:
            record['environment_sha256'] = sha(self.module.canonical(kwargs['environment']))
        if self.fail == stage + '_exit':
            record['exit_code'] = 1
        if self.fail == stage + '_cleanup':
            record['cleanup_complete'] = False
        if self.fail == stage + '_watchdog':
            record['watchdog_stopped'] = True
        if self.fail == stage + '_overflow':
            record['stdout_overflow'] = True
        if self.fail == stage + '_environment':
            record['environment_sha256'] = '0' * 64
        if stage in ('compile', 'link'):
            target = 'object' if stage == 'compile' else 'executable'
            self.opened[fds[target]] = b'' if self.fail == stage + '_empty' else (stage + ' bytes').encode()
            if self.fail == 'archive_changed' and stage == 'link':
                self.opened[fds['link_archives_0']] = b'changed'
            return record, b''
        result = records(self.module)
        if self.fail == 'case_failure':
            result[1]['passed'] = False
            result[1]['checks_executed'] = 3
            record['exit_code'] = 1
        if self.fail == 'case_count_drift':
            result[1]['checks_executed'] -= 1
        if self.fail == 'source_descriptor_changed':
            self.opened[fds['source']] = b'changed'
        if self.fail == 'object_descriptor_changed':
            self.opened[fds['object']] = b'changed'
        if self.fail == 'native_executable_changed':
            self.opened[fds['executable']] = b'changed'
        if self.fail == 'helper_changed':
            path = self.fixture / 'quality_scheduler_cpp_checks.py'
            path.write_bytes(path.read_bytes() + b'\n# changed\n')
        if self.fail == 'case_file_changed':
            path = self.fixture / 'cases/Q28.json'
            path.write_text(path.read_text() + ' ')
        if self.fail == 'receipt_file_changed':
            path = self.fixture / 'receipt.json'
            path.write_text(path.read_text() + ' ')
        if self.fail == 'original_file_changed':
            path = self.fixture / 'original-fixed.json'
            path.write_text(path.read_text() + ' ')
        if self.fail == 'driver_file_changed':
            path = self.fixture / 'q28_depth_verify.py'
            path.write_text(path.read_text() + '\n')
        return record, b'{}' if self.fail == 'malformed_verdict' else encoded(result)


def main():
    checks, scenarios = [], []
    def check(value, label):
        if not value:
            raise AssertionError(label)
        checks.append(label)
    candidate = load(DRIVER)
    before = {name: (GENERATOR if name == candidate.GENERATOR else ROOT / (name + '.py')).read_bytes()
              for name in candidate.SOURCE_BINDINGS}
    for name, data in before.items():
        check(sha(data) == candidate.SOURCE_BINDINGS[name], 'exact_source:' + name)
    check(len(candidate.SPECS) == 4 and len(set(x['case'] for x in candidate.SPECS)) == 4, 'exact_four_vector')
    check(candidate.EXPECTED_CHECKS == tuple((26 * x['fanout'] + 30 if x['kept'] else 14 * x['fanout'] + 24)
                                            for x in candidate.SPECS), 'assertion_vector_from_two_measurements_and_six_checks')
    check(sum(candidate.EXPECTED_CHECKS) == 1708, 'source_derived_assertion_total')
    good = records(candidate)
    check(candidate.parse_records(encoded(good), 0) == good, 'complete_four_records_admitted')
    bad = copy.deepcopy(good)
    bad[0].update(passed=False, checks_executed=0)
    check(candidate.parse_records(encoded(bad), 1) == bad, 'failed_case_preserved_at_zero_checks')
    negatives = []
    for label, mutate in (
        ('missing', lambda x: x.pop()), ('duplicate', lambda x: x.__setitem__(1, copy.deepcopy(x[0]))),
        ('order', lambda x: x.reverse()), ('extra_field', lambda x: x[0].update(raw='forbidden')),
        ('bool_count', lambda x: x[0].update(checks_executed=True)),
        ('bool_counter', lambda x: x[0].update(current_rule_attempts=True)),
        ('short_count', lambda x: x[0].update(checks_executed=x[0]['checks_executed'] - 1)),
        ('wrong_delta', lambda x: x[0].update(old_depth_rule_attempts=x[0]['old_depth_rule_attempts'] + 1)),
        ('zero_work', lambda x: x[0].update(current_rule_attempts=0)),
        ('overflow_counter', lambda x: x[0].update(current_rule_attempts=2 ** 64))):
        value = copy.deepcopy(good)
        mutate(value)
        negatives.append((label, encoded(value), 0))
    negatives += [('wrong_exit', encoded(good), 1), ('empty', b'', 0), ('oversize', b'x' * 16385, 0),
                  ('duplicate_key', encoded(good).replace(b'"case_index": 0', b'"case_index": 0, "case_index": 0', 1), 0)]
    for label, raw, exit_code in negatives:
        try:
            candidate.parse_records(raw, exit_code)
        except (ValueError, TypeError, KeyError):
            check(True, 'parser_rejects:' + label)
        else:
            check(False, 'parser_rejects:' + label)
    source = DRIVER.read_text()
    check('direct.execute(' not in source and 'direct.GENERATORS[' not in source, 'original26_route_not_overridden')
    check('with direct.serial_slot():' in source and "output.open('x')" in source, 'same_serial_lock_and_exclusive_new_evidence')
    check('max_scope_gib=36' in source and 'timeout=120, max_scope_gib=12' in source, 'existing_link_native_memory_limits')
    check(source.index('_seal_native_executable(fds)') < source.index("record, stdout = direct.run_anonymous"), 'sealed_handoff_before_native')
    check("for name in ('worktree', 'receipt', 'original-baseline', 'original-fixed'):" in source
          and "parser.add_argument('--expected-revision', required=True)" in source
          and '--phase' not in source and '--bug' not in source, 'fixed_single_purpose_CLI_without_overrides')
    with tempfile.TemporaryDirectory(prefix='q28-depth-metadata-') as td:
        folder = Path(td)
        def reset():
            for name, data in before.items():
                (folder / (name + '.py')).write_bytes(data)
            (folder / DRIVER.name).write_bytes(DRIVER.read_bytes())
            (folder / 'cases').mkdir(exist_ok=True)
            case = {'id': 'Q28', 'base': candidate.BASE, 'commit': NEW_REVISION}
            (folder / 'cases/Q28.json').write_text(json.dumps(case))
            receipt = {'schema_version': 'production_cpp_closure_v1', 'revision': NEW_REVISION,
                'build': {'build_record_version': 'production_cpp_build_v2', 'revision_before': NEW_REVISION,
                          'revision_after': NEW_REVISION, 'profile': 'rbo', 'target': 'ydb/core/kqp/ut/rbo'},
                'metadata': {'profile': 'rbo', 'target': 'ydb/core/kqp/ut/rbo', 'compile_argv': ['compile'],
                             'compile_cwd': str(folder), 'compile_environment': {}, 'link_cwd': str(folder),
                             'link_environment': {}}, 'files': {}, 'build_record': {}, 'graph_export_record': {}}
            (folder / 'receipt.json').write_text(json.dumps(receipt))
            pair = {'case_count': 26, 'generator_sha256': candidate.SOURCE_BINDINGS['quality_scheduler_cpp_checks']}
            for name in ('original-baseline.json', 'original-fixed.json'):
                (folder / name).write_text(json.dumps(pair))
            return load(folder / DRIVER.name)
        module = reset()
        images = module._sources()
        check(images == before, 'exact_seven_loaded_snapshots')
        saved_modules = dict(sys.modules)
        try:
            helpers = module._helpers(images)
            check(all(sys.modules[name]._verification_source_sha256 == module.SOURCE_BINDINGS[name] for name in images),
                  'all_helpers_loaded_from_checked_bytes')
            check(helpers[0].GENERATORS['Q28'] == 'quality_scheduler_cpp_checks', 'loaded_original26_registration_unchanged')
        finally:
            for name in images:
                if name in saved_modules:
                    sys.modules[name] = saved_modules[name]
                else:
                    sys.modules.pop(name, None)
        for name in images:
            module = reset()
            path = folder / (name + '.py')
            path.write_bytes(path.read_bytes() + b'\n')
            try:
                module._sources()
            except ValueError:
                check(True, 'dependency_drift_rejected:' + name)
            else:
                check(False, 'dependency_drift_rejected:' + name)
        args = SimpleNamespace(worktree=folder, expected_revision=NEW_REVISION,
            receipt=folder / 'receipt.json', original_baseline=folder / 'original-baseline.json',
            original_fixed=folder / 'original-fixed.json')
        failures = [None, 'initial_receipt', 'unaccepted_pair', 'compile_exit', 'compile_cleanup', 'compile_watchdog',
            'compile_overflow', 'compile_environment', 'compile_empty', 'prepare', 'prepare_metadata_error',
            'link_exit', 'link_cleanup', 'link_watchdog', 'link_overflow', 'link_environment', 'link_empty',
            'archive_changed', 'version_verify_1', 'seal', 'native_exit', 'native_cleanup', 'native_watchdog',
            'native_overflow', 'case_failure', 'case_count_drift', 'malformed_verdict', 'version_verify_2',
            'final_receipt', 'source_descriptor_changed', 'object_descriptor_changed', 'native_executable_changed', 'helper_changed', 'case_file_changed',
            'receipt_file_changed', 'original_file_changed', 'driver_file_changed',
            'compile_cancel', 'prepare_cancel', 'link_cancel', 'seal_cancel', 'after_seal_cancel', 'native_cancel',
            'version_verify_2_cancel', 'final_receipt_cancel']
        for failure in failures:
            module = reset()
            fake = FakeRun(module, folder, fail=failure)
            # Revision/tree/source logic gets separate mocks below; the compile/link/native path uses fake FDs only.
            revision = lambda *ignored: (NEW_REVISION, (folder / 'cases/Q28.json').read_bytes(),
                json.loads((folder / 'cases/Q28.json').read_bytes()))
            with patch.object(module, '_revision', revision), patch.object(module.os, 'close', fake.close):
                evidence, code = module.execute(args, fake.direct, fake.metadata, fake.version, fake.generator)
            check(not fake.opened, 'no_descriptor_leak:' + str(failure))
            check(evidence['full_q28_admission'] is False and evidence['query_semantics_verified'] is False
                  and evidence['quality_improvement_verified'] is False, 'scope_not_promoted:' + str(failure))
            expected = 0 if failure is None else 2 if failure == 'case_failure' else 130 if failure.endswith('_cancel') else 1
            check(code == expected, 'failure_disposition:' + str(failure))
            check(evidence['amendment_local_contracts_verified'] is (failure is None), 'admission:' + str(failure))
            if failure is None or failure in ('case_failure', 'final_receipt', 'final_receipt_cancel',
                                              'source_descriptor_changed', 'object_descriptor_changed', 'native_executable_changed', 'helper_changed', 'case_file_changed',
                                              'receipt_file_changed', 'original_file_changed', 'driver_file_changed'):
                check(len(evidence.get('cases', [])) == 4, 'completed_verdict_retained:' + str(failure))
            if failure == 'prepare_metadata_error':
                check(evidence['production_version_infrastructure']['failure_recorded'], 'version_failure_retained')
            scenarios.append({'scenario': str(failure), 'exit_code': code, 'stages': [x[0] for x in fake.commands],
                              'descriptors_closed': len(fake.closed), 'checks_passed': True})
        for allocation in range(1, 8):
            module = reset()
            fake = FakeRun(module, folder, allocation_failure=allocation)
            with patch.object(module, '_revision', revision), patch.object(module.os, 'close', fake.close):
                evidence, code = module.execute(args, fake.direct, fake.metadata, fake.version, fake.generator)
            check(code == 1 and not fake.opened and evidence['amendment_local_contracts_verified'] is False,
                  'allocation_interruption_cleanup:' + str(allocation))
            scenarios.append({'scenario': 'allocation_' + str(allocation), 'exit_code': code,
                              'descriptors_closed': len(fake.closed), 'checks_passed': True})
        module = reset()
        fake = FakeRun(module, folder)
        production = {path: ('bound production image ' + path).encode() for path in module.PRODUCTION_BINDINGS}
        module.PRODUCTION_BINDINGS = {path: sha(raw) for path, raw in production.items()}
        def git(command, **kwargs):
            if command[1] == 'diff':
                return module.HEADER + '\n'
            if command[1] == 'show':
                return production[command[2].split(':', 1)[1]]
            raise AssertionError('unexpected mocked Git command')
        with patch.object(module.subprocess, 'check_output', git):
            check(module._revision(args, fake.direct, fake.metadata)[0] == NEW_REVISION, 'exact_fixed_revision_tree_and_sources')
            for bad in (module.BASE, module.OLD_FIXED, 'bad', 'A' * 40):
                altered = copy.copy(args)
                altered.expected_revision = bad
                try:
                    module._revision(altered, fake.direct, fake.metadata)
                except ValueError:
                    check(True, 'revision_rejected:' + bad[:8])
                else:
                    check(False, 'revision_rejected:' + bad[:8])
            production[next(iter(production))] += b' changed'
            try:
                module._revision(args, fake.direct, fake.metadata)
            except ValueError:
                check(True, 'production_source_drift_rejected')
            else:
                check(False, 'production_source_drift_rejected')
        with patch.object(module.subprocess, 'check_output', lambda *a, **kw: module.HEADER + '\nextra.cpp\n'):
            try:
                module._revision(args, fake.direct, fake.metadata)
            except ValueError:
                check(True, 'extra_production_change_rejected')
            else:
                check(False, 'extra_production_change_rejected')
    for name, data in before.items():
        path = GENERATOR if name == candidate.GENERATOR else ROOT / (name + '.py')
        check(path.read_bytes() == data, 'live_source_unchanged:' + name)
    output = ROOT / 'verification/Q28-depth-fixed-runner-metadata-checks-20261028.json'
    report = {'schema': 'q28_depth_fixed_runner_mock_checks_v1', 'checks_passed': len(checks), 'checks': checks,
              'scenarios': scenarios, 'scenario_count': len(scenarios), 'source_sha256': sha(DRIVER.read_bytes()),
              'native_build_toolchain_runs': 0, 'generated_CPP_or_query_payloads_persisted': False,
              'actual_native_verification_pending': True}
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'checks_passed': len(checks), 'scenarios': len(scenarios), 'report_sha256': sha(output.read_bytes())}))


if __name__ == '__main__':
    main()
