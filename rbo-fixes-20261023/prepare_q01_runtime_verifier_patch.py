"""Prepare Q01 runtime integration for an idle driver boundary; no file edits."""
import difflib
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def _once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('runtime_integration_anchor_is_not_unique')
    return source.replace(before, after, 1)


def render_files():
    result = {}
    def change(name, edit):
        before = (ROOT / name).read_text()
        result[name] = (before, edit(before))
    change('anonymous_cpp_metadata.py', lambda source: _once(source, "PROFILES = {\n", "PROFILES = {\n    'kqp_runtime': {'target': 'ydb/core/kqp/runtime/streaming/ut', 'sample': 'ydb/core/kqp/runtime/streaming/kqp_streaming_aggregation_ut.cpp'},\n"))
    def verifier(source):
        source = _once(source, "GENERATORS = {'Q14':", "GENERATORS = {'Q01': 'quality_range_runtime_cpp_generator', 'Q14':")
        source = _once(source, 'def _expected_revision(bug, phase, worktree):', "def _closure_profile(bug):\n    return 'kqp_runtime' if bug == 'Q01' else 'ast' if bug == 'Q27' else 'rbo'\n\n\ndef _expected_revision(bug, phase, worktree):")
        source = _once(source, "if bug == 'Q15' and phase != 'fixed':", "if bug in ('Q01', 'Q15') and phase != 'fixed':")
        source = _once(source, "'q15_new_helper_requires_fixed_only_ir_admission'", "'new_runtime_or_helper_requires_fixed_only_admission'")
        expression = "'ast' if args.bug == 'Q27' else 'rbo'"
        if source.count(expression) != 3:
            raise ValueError('unexpected_closure_profile_selection_count')
        source = source.replace(expression, '_closure_profile(args.bug)')
        source = _once(source, "elif bug in ('Q15', 'Q25', 'Q28'):", "elif bug in ('Q01', 'Q15', 'Q25', 'Q28'):")
        source = _once(source, 'def parse_verdicts(stdout, count, exit_code, bug):\n', "def parse_verdicts(stdout, count, exit_code, bug):\n    if bug == 'Q01':\n        from quality_range_runtime_contract import parse_verdicts as parse_runtime_verdicts\n        return parse_runtime_verdicts(stdout, count, exit_code)\n")
        source = _once(source, "'proof_scope': 'source_extracted_helper' if args.bug == 'Q27' else 'linked_production_local_contracts'", "'proof_scope': ('linked_production_runtime_lifecycle_contracts' if args.bug == 'Q01' else\n                                'source_extracted_helper' if args.bug == 'Q27' else 'linked_production_local_contracts')")
        source = _once(source, "    if bug == 'Q15':\n        raise MetadataError('q15_uses_fixed_ir_admission_and_separately_paired_sql')", "    if bug in ('Q01', 'Q15'):\n        raise MetadataError('fixed_admission_requires_separately_paired_sql')")
        source = _once(source, '    if bug != \'Q15\' or not isinstance(evidence, dict):\n', "    if bug == 'Q01':\n        from quality_range_runtime_contract import validate_fixed_runtime\n        return validate_fixed_runtime(evidence, revision)\n    if bug != 'Q15' or not isinstance(evidence, dict):\n")
        return source
    change('anonymous_cpp_verify.py', verifier)
    def driver(source):
        source = _once(source, "if bug != 'Q15' or bug not in args.bugs or bug in ir_evidence:", "if bug not in ('Q01', 'Q15') or bug not in args.bugs or bug in ir_evidence:")
        source = _once(source, 'Expected one direct IR evidence record for requested Q15', 'Expected one fixed runtime/IR evidence record for each requested Q01 or Q15')
        return _once(source, 'Direct IR admission evidence does not match the complete fixed Q15 contract', 'Direct admission evidence does not match the complete fixed runtime/IR contract')
    change('verify_series.py', driver)
    def range_checks(source):
        source = _once(source,
            "'quality_requires_separate_plan_evidence': True, 'optimization_acceptance_case': accepted,",
            "'quality_requires_separate_plan_evidence': True, 'optimization_acceptance_case': accepted,\n            'additional_ir_admission_required': accepted,")
        return _once(source,
            'optimization_acceptance_case=accepted, quality_improvement_verified=False,',
            'optimization_acceptance_case=accepted, quality_improvement_verified=False,\n                   additional_ir_admission_required=accepted,')
    change('quality_range_minmax_checks.py', range_checks)
    def minmax_checks(source):
        source = _once(source,
            "'optimization_acceptance_case': acceptance,\n",
            "'optimization_acceptance_case': acceptance,\n            'additional_ir_admission_required': control == 'range',\n")
        return _once(source,
            'optimization_acceptance_case=acceptance, quality_improvement_verified=False,',
            "optimization_acceptance_case=acceptance, quality_improvement_verified=False,\n                   additional_ir_admission_required=spec.get('control') == 'range',")
    change('quality_minmax_checks.py', minmax_checks)
    return result


def render_patch():
    return ''.join('diff --git a/' + name + ' b/' + name + '\n' +
                   ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + name, tofile='b/' + name))
                   for name, (before, after) in render_files().items())
