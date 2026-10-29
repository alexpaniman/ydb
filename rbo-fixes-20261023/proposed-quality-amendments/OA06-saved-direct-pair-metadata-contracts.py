"""Negative metadata-only checks for the dormant saved direct-pair adapter.

Applies reviewed patches to temporary Python copies. No workload renderer,
compiler, linker, native verifier or graph generator is executed; no native
pass evidence is synthesized. A metadata-only renderer stand-in checks snapshot
hash transport. Receipt fixtures contain metadata only and are temporary.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    sys.dont_write_bytecode = True
    root = Path(__file__).resolve().parent.parent
    prerequisites = [root / 'proposed-correctness-amendments' / name for name in (
        'C47-narrow-production-export-artifacts.patch',
        'C47-production-graph-lifecycle-artifacts.patch',
        'C31-stdout-link-output-artifacts.patch')]
    prerequisites += [root / 'proposed-quality-amendments' / name for name in (
        'Q19-abi-stable-inspection-objects.patch',
        'Q11-direct-union-admission-artifacts.patch',
        'OA06-saved-direct-pair-admission.patch')]
    summary = {'scope': 'negative_metadata_only', 'build_or_native_executed': False,
               'workload_renderers_called': False, 'fabricated_positive_native_evidence': False}
    with tempfile.TemporaryDirectory(prefix='oa06-metadata-contracts-') as directory:
        base = Path(directory)
        here = base / 'rbo-fixes-20261023'
        here.mkdir()
        for name in ('anonymous_cpp_verify.py', 'anonymous_cpp_metadata.py'):
            shutil.copyfile(root / name, here / name)
        for path in prerequisites:
            result = subprocess.run(['git', 'apply', str(path)], cwd=base, capture_output=True)
            if result.returncode:
                raise RuntimeError('dormant patch application failed')
        sys.path[:0] = [str(here), str(root)]
        spec = importlib.util.spec_from_file_location('oa06_reviewed_helper', here / 'anonymous_cpp_verify.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        rejected = 0

        def forbidden(*args, **kwargs):
            raise AssertionError('metadata contract attempted rendering or execution')

        helper.run_anonymous = forbidden
        helper._read_tool = forbidden

        def reject(call):
            nonlocal rejected
            try:
                call()
            except helper.MetadataError:
                rejected += 1
            else:
                raise AssertionError('invalid saved metadata was accepted')

        vectors = {}
        for bug in helper.PAIRED_DIRECT_BUGS:
            name = helper.GENERATORS[bug] + '.py'
            shutil.copyfile(root / name, here / name)
            module, digest, vector = helper._current_pair_contract(bug)
            module.render_cpp_suite = forbidden
            vectors[bug] = {'generator_sha256': digest, 'case_count': len(vector['case_names']),
                            'case_vector_sha256': helper._json_digest(vector)}
            metadata = {'generator_sha256': digest, 'case_count': len(vector['case_names']),
                        'direct_case_vector_sha256': helper._json_digest(vector)}
            for key, value in (('generator_sha256', '0' * 64), ('case_count', metadata['case_count'] - 1),
                               ('case_count', True), ('direct_case_vector_sha256', None),
                               ('direct_case_vector_sha256', '0' * 64)):
                invalid = dict(metadata, **{key: value})
                reject(lambda: helper._validate_saved_pair_contract(
                    invalid, bug, '0' * 40, module, digest, vector))
            # A matching generator/vector with no referenced receipt must fail
            # before invoking any renderer, especially Q19's dynamic wrapper.
            reject(lambda: helper._validate_saved_pair_contract(
                metadata, bug, '0' * 40, module, digest, vector))
        summary['current_case_vectors'] = vectors

        # Preserve the loaded snapshot hash even if the file changes while a
        # metadata-only stand-in runs. No production adapter is rendered here.
        source_path = here / (helper.GENERATORS['Q14'] + '.py')
        snapshot = source_path.read_bytes()
        snapshot_hash = hashlib.sha256(snapshot).hexdigest()
        original_loader = helper._current_pair_contract

        def metadata_loader(bug):
            module, digest, vector = original_loader(bug)

            def metadata_only_render(*args):
                source_path.write_bytes(snapshot + b'\n# metadata-only freshness check\n')
                return 'metadata-only snapshot transport'

            module.render_cpp_suite = metadata_only_render
            return module, digest, vector

        helper._current_pair_contract = metadata_loader
        try:
            payload, count, returned_hash = helper.render_suite('Q14', root.parent, '0' * 40)
            del payload
            _, next_hash, next_vector = original_loader('Q14')
            if (returned_hash != snapshot_hash or returned_hash == next_hash
                or count != len(next_vector['case_names'])):
                raise AssertionError('render route lost the exact loaded snapshot hash')
        finally:
            helper._current_pair_contract = original_loader
            source_path.write_bytes(snapshot)
        summary['metadata_only_snapshot_hash_probe'] = 'passed; changed bytes with unchanged case count rejected by next-snapshot comparison'

        # Every fixture below has an invalid guard field. No complete process
        # evidence or native pass verdict is fabricated or accepted.
        guard_start = rejected
        for phase in ('compile', 'link', 'native'):
            valid_shape = {'exit_code': 0, 'watchdog_stopped': False,
                           'stdout_overflow': False, 'cleanup_complete': True}
            for key in valid_shape:
                invalid = dict(valid_shape)
                del invalid[key]
                reject(lambda: helper._validate_saved_pair_process(invalid, phase))
            invalid_fields = [('exit_code', value) for value in (False, True, 0.0, '0', None, 2)]
            invalid_fields += [(key, value) for key in ('watchdog_stopped', 'stdout_overflow')
                               for value in (None, 0, True)]
            invalid_fields += [('cleanup_complete', value) for value in (None, 1, False)]
            if phase != 'native':
                invalid_fields.append(('exit_code', 1))
            for key, value in invalid_fields:
                invalid = dict(valid_shape, **{key: value})
                reject(lambda: helper._validate_saved_pair_process(invalid, phase))
            for invalid in (None, [], 0):
                reject(lambda: helper._validate_saved_pair_process(invalid, phase))
        summary['invalid_guard_metadata_rejections'] = rejected - guard_start

        # Read genuine retained receipt metadata; never relabel it as another
        # revision, pair it with fabricated verdicts or claim runtime acceptance.
        receipt_path = root / 'verification/cpp-closures/24e1aae9a067-1792736142124693334/receipt.json'
        raw = receipt_path.read_bytes()
        receipt = json.loads(raw)
        revision = receipt['revision']
        observed = {'closure_receipt_path': str(receipt_path),
                    'closure_receipt_sha256': hashlib.sha256(raw).hexdigest()}
        helper._saved_pair_receipt(observed, 'Q14', revision)
        summary['genuine_retained_receipt_metadata_read'] = True
        reject(lambda: helper._saved_pair_receipt(dict(observed, closure_receipt_sha256='0' * 64), 'Q14', revision))
        reject(lambda: helper._saved_pair_receipt(observed, 'Q14', '0' * 40))
        reject(lambda: helper._saved_pair_receipt(observed, 'Q27', revision))
        changes = [(['schema_version'], 'unknown'), (['revision'], '0' * 40),
                   (['build', 'revision'], '0' * 40), (['build', 'exit_code'], True),
                   (['build', 'profile'], 'ast'), (['build', 'target'], 'wrong-target'),
                   (['metadata', 'profile'], 'ast'), (['metadata', 'target'], 'wrong-target'),
                   (['files'], {}), (['worktree'], 'relative-path'),
                   (['worktree'], str(base)),
                   (['build', 'build_record_version'], 'unknown'),
                   (['build', 'build_record_version'], 'production_cpp_build_v2')]
        path = base / 'invalid-receipt.json'
        for keys, value in changes:
            invalid = copy.deepcopy(receipt)
            parent = invalid
            for key in keys[:-1]:
                parent = parent[key]
            parent[keys[-1]] = value
            raw = json.dumps(invalid).encode()
            path.write_bytes(raw)
            evidence = {'closure_receipt_path': str(path),
                        'closure_receipt_sha256': hashlib.sha256(raw).hexdigest()}
            reject(lambda: helper._saved_pair_receipt(evidence, 'Q14', revision))
        for wrapping in (None, {}, {'both_caller_relocations_verified': True},
                         {'both_caller_relocations_verified': True, 'request_symbols': {}, 'request_symbol_sha256': {}},
                         {'both_caller_relocations_verified': True,
                          'request_symbols': {'single': None, 'tuple': None},
                          'request_symbol_sha256': {'single': '0' * 64, 'tuple': '0' * 64}}):
            reject(lambda: helper._saved_q19_symbols({'wrapping': wrapping}, receipt))
        summary['invalid_metadata_rejections'] = rejected
        summary['temporary_patch_application_passed'] = True
        summary['helper_after_sha256'] = hashlib.sha256((here / 'anonymous_cpp_verify.py').read_bytes()).hexdigest()
    return summary


if __name__ == '__main__':
    print(json.dumps(main(), sort_keys=True))
