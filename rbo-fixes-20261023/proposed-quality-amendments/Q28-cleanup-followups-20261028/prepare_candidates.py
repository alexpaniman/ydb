"""Prepare separate dormant harness repairs; never execute native/build work."""
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DIRECT_BEFORE = '0ee20f0c2c31f3e36e9002373d501a9f465c9cdcbabec1d56d64d439fec2c60d'
RUNNER_BEFORE = '77b151edb0de8cfa0c73ea44f0e230513bfcc802d3d14ecaf51a512aafc06d96'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def changed_function(raw, name, replacement):
    source = raw.decode()
    node = next(node for node in ast.parse(source).body
                if isinstance(node, ast.FunctionDef) and node.name == name)
    lines = source.splitlines(True)
    return (''.join(lines[:node.lineno - 1]) + replacement
            + ''.join(lines[node.end_lineno:])).encode()


def patch_file(name, target, before, after):
    path = ROOT / 'proposed-quality-amendments' / name
    path.write_text('diff --git a/' + target + ' b/' + target + '\n'
        + ''.join(difflib.unified_diff(before.decode().splitlines(True), after.decode().splitlines(True),
            fromfile='a/' + target, tofile='b/' + target)))
    with tempfile.TemporaryDirectory(prefix='q28-cleanup-patch-') as temp:
        destination = Path(temp) / target
        destination.parent.mkdir(parents=True)
        destination.write_bytes(before)
        for command in (['git', 'apply', '--check', '--whitespace=error-all', str(path)],
                        ['git', 'apply', str(path)]):
            subprocess.run(command, cwd=temp, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert destination.read_bytes() == after
    return {'path': str(path.relative_to(ROOT)), 'sha256': sha(path.read_bytes()),
            'target': target, 'before_sha256': sha(before), 'after_sha256': sha(after)}


def main():
    direct = (ROOT / 'anonymous_cpp_verify.py').read_bytes()
    runner_path = ROOT / 'proposed-quality-amendments/Q28-depth-runner-20261028/q28_depth_verify.py'
    runner = runner_path.read_bytes()
    assert sha(direct) == DIRECT_BEFORE and sha(runner) == RUNNER_BEFORE
    direct_after = changed_function(direct, '_memfd', '''def _memfd(name, data=b'', executable=False):
    fd = os.memfd_create(name, os.MFD_CLOEXEC | (os.MFD_ALLOW_SEALING if executable else 0))
    try:
        if data:
            pending = memoryview(data)
            while pending:
                written = os.write(fd, pending)
                if written <= 0:
                    raise OSError('anonymous_memfd_write_made_no_progress')
                pending = pending[written:]
        os.lseek(fd, 0, os.SEEK_SET)
        if executable:
            os.fchmod(fd, 0o700)
        return fd
    except BaseException:
        os.close(fd)
        raise
''')
    before_text = "        evidence.update(failure_stage='source_or_serial_guard', failure_class=type(error).__name__)"
    after_text = """        evidence.update(status='q28_depth_harness_failed',
                        amendment_local_contracts_verified=False,
                        failure_stage='source_or_serial_guard', failure_class=type(error).__name__)"""
    assert runner.decode().count(before_text) == 1
    runner_after = runner.decode().replace(before_text, after_text).encode()
    for name, raw in (('anonymous_cpp_verify.py', direct_after), ('q28_depth_verify.py', runner_after)):
        ast.parse(raw)
        (HERE / name).write_bytes(raw)
    records = [
        patch_file('Q28-anonymous-memfd-initialization-cleanup-20261028.patch',
                   ROOT.name + '/anonymous_cpp_verify.py', direct, direct_after),
        patch_file('Q28-depth-serial-release-admission-20261028.patch',
                   ROOT.name + '/q28_depth_verify.py', runner, runner_after)]
    (HERE / 'source-images.json').write_text(json.dumps({'patches': records}, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'patches': records}, sort_keys=True))


if __name__ == '__main__':
    main()
