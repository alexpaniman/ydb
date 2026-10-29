"""Prepare C31/C47 fixed-only integration without modifying live helpers."""
import difflib
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def _once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('correctness_cpp_patch_anchor_is_not_unique')
    return text.replace(before, after, 1)


def render_files():
    path = ROOT/'anonymous_cpp_verify.py'
    before = source = path.read_text()
    source = _once(source, "GENERATORS = {'Q01':", "GENERATORS = {'C31': 'c31_identity_cpp_generator', 'C47': 'c47_lookup_copy_contract_generator',\n              'Q01':")
    source = _once(source, "    if bug in ('Q01', 'Q15') and phase != 'fixed':", "    if bug in ('C31', 'C47', 'Q01', 'Q15') and phase != 'fixed':")
    source = _once(source, 'def render_suite(bug, worktree, revision, wrapper_symbols=None):\n',
                   "def render_suite(bug, worktree, revision, wrapper_symbols=None):\n    if bug in ('C31', 'C47'):\n        from correctness_cpp_contract import render_suite as render_correctness_suite\n        return render_correctness_suite(bug, revision)\n")
    source = _once(source, 'def parse_verdicts(stdout, count, exit_code, bug):\n',
                   "def parse_verdicts(stdout, count, exit_code, bug):\n    if bug in ('C31', 'C47'):\n        from correctness_cpp_contract import parse_verdicts as parse_correctness_verdicts\n        return parse_correctness_verdicts(stdout, count, exit_code)\n")
    source = _once(source, '    del source\n    try:\n',
                   "    del source\n    try:\n        if args.bug in ('C31', 'C47'):\n            from correctness_cpp_contract import profile_digest\n            evidence['direct_contract_profile_sha256'] = profile_digest(args.bug, revision)\n")
    source = _once(source, "        return evidence, 0 if evidence['status'] == 'direct_local_contracts_passed' else 2\n",
                   "        if args.bug in ('C31', 'C47') and evidence['status'] == 'direct_local_contracts_passed':\n            if not validate_fixed_admission(evidence, args.bug, revision):\n                raise MetadataError('incomplete_fixed_correctness_cpp_admission')\n        return evidence, 0 if evidence['status'] == 'direct_local_contracts_passed' else 2\n")
    source = _once(source, "    if bug in ('Q01', 'Q15'):\n        raise MetadataError('fixed_admission_requires_separately_paired_sql')",
                   "    if bug in ('C31', 'C47', 'Q01', 'Q15'):\n        raise MetadataError('fixed_admission_requires_separately_paired_sql')")
    source = _once(source, '    """Accept only complete native fixed-only Q15 contracts for SQL admission."""\n',
                   '    """Validate fixed local contracts; query semantics require separate evidence."""\n' +
                   "    if bug in ('C31', 'C47'):\n        from correctness_cpp_contract import validate_fixed_local_contracts\n        return validate_fixed_local_contracts(evidence, bug, revision)\n")
    return {'anonymous_cpp_verify.py': (before, source)}


def render_patch():
    return ''.join('diff --git a/'+name+' b/'+name+'\n'+''.join(difflib.unified_diff(
        before.splitlines(True),after.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
        for name,(before,after) in render_files().items())
