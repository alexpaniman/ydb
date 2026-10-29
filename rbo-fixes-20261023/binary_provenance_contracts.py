"""Metadata-only runner receipt contracts; never execute or hash a real runner."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import binary_provenance as provenance


def run_contract_checks():
    assertions = 0
    def check(value):
        nonlocal assertions
        assertions += 1
        if not value:
            raise AssertionError('Runner receipt contract failed')
    def rejected(call, reason):
        try:
            call()
        except provenance.ProvenanceError as error:
            check(str(error) == reason)
        else:
            check(False)

    revision = '1' * 40
    recorded = {'resolved_path': '/metadata/runner', 'device': 7, 'inode': 31,
                'size_bytes': 8, 'mtime_ns': 101, 'ctime_ns': 103, 'sha256': '2' * 64}
    with tempfile.TemporaryDirectory(prefix='rbo-receipt-contract-') as directory:
        worktree = Path(directory).resolve()
        path = worktree / 'receipt.json'
        receipt = {'schema_version': provenance.VERSION, 'worktree': str(worktree),
                   'revision': revision, 'binary': recorded,
                   'build': {'exit_code': 0, 'target': provenance.BUILD_TARGET}}
        def verify(document, current):
            raw = json.dumps(document, sort_keys=True).encode()
            path.write_bytes(raw)
            with patch.object(provenance, '_source_revision', return_value=revision), \
                 patch.object(provenance, 'fingerprint_binary', return_value=current):
                return provenance.verify_build_receipt(worktree, revision, path), raw
        result, raw = verify(receipt, dict(recorded))
        check(result['binary_provenance_verified'] is True and result['build_receipt_ctime_changed'] is False)
        check(result['build_receipt_sha256'] == hashlib.sha256(raw).hexdigest())
        current = dict(recorded, ctime_ns=107)
        result, raw = verify(receipt, current)
        check(result['binary_provenance_verified'] is True and result['build_receipt_ctime_changed'] is True)
        check(result['binary_provenance_comparison_version'] == 'receipt_ctime_only_revalidation_v1')
        check(result['binary_provenance'] == current and result['binary_provenance']['ctime_ns'] == 107)
        check(path.read_bytes() == raw and result['build_receipt_sha256'] == hashlib.sha256(raw).hexdigest())
        for field in ('resolved_path', 'device', 'inode', 'size_bytes', 'mtime_ns', 'sha256'):
            changed = dict(current)
            changed[field] = changed[field] + 1 if type(changed[field]) is int else changed[field] + 'changed'
            rejected(lambda: verify(receipt, changed), 'runner_does_not_match_successful_build_receipt')
        for field in recorded:
            missing = copy.deepcopy(receipt)
            missing['binary'].pop(field)
            rejected(lambda: verify(missing, current), 'runner_does_not_match_successful_build_receipt')
        extra = copy.deepcopy(receipt); extra['binary']['unrecognized_identity'] = 1
        rejected(lambda: verify(extra, current), 'runner_does_not_match_successful_build_receipt')
        for invalid in (None, True, 103.0, '103', -1):
            invalid_receipt = copy.deepcopy(receipt); invalid_receipt['binary']['ctime_ns'] = invalid
            rejected(lambda: verify(invalid_receipt, current), 'runner_does_not_match_successful_build_receipt')
        invalid_receipt = copy.deepcopy(receipt); invalid_receipt['binary'] = None
        rejected(lambda: verify(invalid_receipt, current), 'runner_does_not_match_successful_build_receipt')
        for build in ({'exit_code': 1, 'target': provenance.BUILD_TARGET}, {'exit_code': 0, 'target': 'unrelated'}):
            invalid_receipt = dict(receipt, build=build)
            rejected(lambda: verify(invalid_receipt, current), 'build_receipt_does_not_confirm_successful_runner_build')
        invalid_receipt = dict(receipt, revision='3' * 40)
        rejected(lambda: verify(invalid_receipt, current), 'build_receipt_source_identity_mismatch')
        rejected(lambda: provenance.verify_build_receipt(worktree, revision, worktree / 'absent.json'),
                 'matching_successful_build_receipt_missing')
        # Per-query checking must use the new snapshot, including its ctime.
        fresh_identity = {key: value for key, value in current.items() if key != 'sha256'}
        with patch.object(provenance, '_source_revision', return_value=revision), \
             patch.object(provenance, 'binary_identity', return_value=fresh_identity):
            check(provenance.unchanged_since_verification(worktree, revision, result))
        with patch.object(provenance, '_source_revision', return_value=revision), \
             patch.object(provenance, 'binary_identity', return_value=dict(fresh_identity, ctime_ns=109)):
            check(not provenance.unchanged_since_verification(worktree, revision, result))

    # Exercise the unchanged strict fingerprint rule with metadata-only mocks.
    def fingerprint(before_ctime, after_ctime, path_ctime):
        root, candidate, resolved, stream = (MagicMock() for _ in range(4))
        root.__truediv__.return_value = candidate
        candidate.resolve.return_value = resolved
        resolved.__str__.return_value = '/metadata/runner'
        resolved.open.return_value.__enter__.return_value = stream
        stream.fileno.return_value = 17
        stream.read.side_effect = [b'metadata', b'']
        def stat(ctime):
            return SimpleNamespace(st_dev=7, st_ino=31, st_size=8, st_mtime_ns=101, st_ctime_ns=ctime)
        identity = {key: value for key, value in recorded.items() if key != 'sha256'}
        identity['ctime_ns'] = path_ctime
        with patch.object(provenance, 'Path', return_value=root), \
             patch.object(provenance.os, 'fstat', side_effect=[stat(before_ctime), stat(after_ctime)]), \
             patch.object(provenance, 'binary_identity', return_value=identity):
            return provenance.fingerprint_binary('/metadata')
    check(fingerprint(103, 103, 103)['sha256'] == hashlib.sha256(b'metadata').hexdigest())
    rejected(lambda: fingerprint(103, 107, 107), 'runner_changed_during_fingerprint')
    rejected(lambda: fingerprint(103, 103, 107), 'runner_changed_during_fingerprint')
    return {'status': 'metadata_contracts_passed_native_unexecuted', 'assertions': assertions,
            'real_runner_fingerprints': 0, 'native_runs': 0, 'builds': 0}


if __name__ == '__main__':
    print(json.dumps(run_contract_checks(), sort_keys=True))
