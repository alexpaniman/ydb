"""Bind native evidence to a successfully built executable, without loading it in RAM."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
RUNNER = 'ydb/tests/tools/kqprun/kqprun'
BUILD_TARGET = 'ydb/tests/tools/kqprun'
VERSION = 'successful_runner_build_v1'


class ProvenanceError(Exception):
    """Safe reason codes only; no build diagnostics or workload contents."""


def _source_revision(worktree):
    revision = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=worktree, text=True).strip()
    if subprocess.check_output(
            ['git', 'status', '--porcelain', '--untracked-files=no'], cwd=worktree):
        raise ProvenanceError('tracked_sources_are_modified')
    return revision


def _identity(stat):
    return {'size_bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns,
            'ctime_ns': stat.st_ctime_ns, 'inode': stat.st_ino, 'device': stat.st_dev}


def binary_identity(worktree):
    resolved = (Path(worktree) / RUNNER).resolve(strict=True)
    return dict(_identity(resolved.stat()), resolved_path=str(resolved))


def fingerprint_binary(worktree):
    candidate = Path(worktree) / RUNNER
    resolved = candidate.resolve(strict=True)
    digest = hashlib.sha256()
    with resolved.open('rb') as stream:
        before = _identity(os.fstat(stream.fileno()))
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
        after = _identity(os.fstat(stream.fileno()))
    identity = dict(after, resolved_path=str(resolved))
    if before != after or binary_identity(worktree) != identity:
        raise ProvenanceError('runner_changed_during_fingerprint')
    return dict(identity, sha256=digest.hexdigest())


def receipt_path(worktree, revision):
    directory_digest = hashlib.sha256(str(Path(worktree).resolve()).encode()).hexdigest()[:16]
    return HERE / 'verification' / 'build-receipts' / (revision + '-' + directory_digest + '.json')


def write_build_receipt(worktree, revision, build_record):
    """Call immediately after the successful build, before other source/build work."""
    worktree = Path(worktree).resolve()
    if build_record.get('exit_code') != 0:
        raise ProvenanceError('unsuccessful_build_cannot_issue_receipt')
    targets = build_record.get('target', '').split()
    if BUILD_TARGET not in targets:
        raise ProvenanceError('successful_build_did_not_include_runner')
    if build_record.get('revision') != revision or _source_revision(worktree) != revision:
        raise ProvenanceError('build_source_revision_mismatch')
    binary = fingerprint_binary(worktree)
    if _source_revision(worktree) != revision:
        raise ProvenanceError('sources_changed_during_fingerprint')
    receipt = {'schema_version': VERSION, 'worktree': str(worktree),
               'revision': revision, 'recorded_unix_ns': time.time_ns(), 'binary': binary,
               'build': {key: build_record[key] for key in (
                   'exit_code', 'target', 'elapsed_seconds', 'diagnostic_sha256') if key in build_record}}
    path = receipt_path(worktree, revision)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(receipt, sort_keys=True, indent=2) + '\n')
    temporary.replace(path)
    return path


def verify_build_receipt(worktree, revision, supplied_path=None):
    worktree = Path(worktree).resolve()
    path = Path(supplied_path) if supplied_path else receipt_path(worktree, revision)
    if not path.is_file():
        raise ProvenanceError('matching_successful_build_receipt_missing')
    raw = path.read_bytes()
    receipt = json.loads(raw)
    if receipt.get('schema_version') != VERSION:
        raise ProvenanceError('unsupported_build_receipt_version')
    if receipt.get('worktree') != str(worktree) or receipt.get('revision') != revision:
        raise ProvenanceError('build_receipt_source_identity_mismatch')
    if _source_revision(worktree) != revision:
        raise ProvenanceError('current_source_revision_mismatch')
    build = receipt.get('build', {})
    if build.get('exit_code') != 0 or BUILD_TARGET not in build.get('target', '').split():
        raise ProvenanceError('build_receipt_does_not_confirm_successful_runner_build')
    binary = fingerprint_binary(worktree)
    expected = receipt.get('binary')
    # Completed snapshots may differ only in ctime after metadata/hardlink
    # changes. fingerprint_binary still checks ctime throughout the full read.
    # Require the complete receipt identity and exact content/stable identity;
    # retain the freshly measured ctime for per-query mutation checks below.
    if (not isinstance(expected, dict) or expected.keys() != binary.keys()
            or type(expected.get('ctime_ns')) is not int or expected['ctime_ns'] < 0
            or any(binary[key] != expected[key] for key in binary if key != 'ctime_ns')):
        raise ProvenanceError('runner_does_not_match_successful_build_receipt')
    return {'binary_provenance_version': VERSION, 'binary_provenance_verified': True,
            'binary_provenance_comparison_version': 'receipt_ctime_only_revalidation_v1',
            'build_receipt_ctime_changed': binary['ctime_ns'] != expected['ctime_ns'],
            'binary_provenance': binary, 'build_receipt_path': str(path.resolve()),
            'build_receipt_sha256': hashlib.sha256(raw).hexdigest()}


def unchanged_since_verification(worktree, revision, provenance):
    expected = {key: value for key, value in provenance['binary_provenance'].items() if key != 'sha256'}
    try:
        return binary_identity(worktree) == expected and _source_revision(worktree) == revision
    except (OSError, subprocess.SubprocessError, ProvenanceError):
        return False
