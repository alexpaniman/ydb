"""Prepare mode-selection repair for an idle boundary; never edit live helpers."""
import difflib
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def _once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('optimizer_mode_patch_anchor_is_not_unique')
    return text.replace(before, after, 1)


def render_files():
    changes = {}
    def change(name, edit):
        before = (ROOT/name).read_text()
        changes[name] = (before, edit(before))
    def native(source):
        source = _once(source, 'import importlib.util\n', 'import importlib.util\nimport inspect\n')
        source = _once(source, 'from verification_contract import failure_stage, parse_rows_output\n',
                       'from verification_contract import failure_stage, parse_rows_output\nfrom optimizer_mode_contract import optimizer_mode_configuration\n')
        source = _once(source, "    if hasattr(module, 'metadata'):\n        return module.metadata(case, n)",
                       "    if hasattr(module, 'metadata'):\n        if 'mode' in inspect.signature(module.metadata).parameters:\n            return module.metadata(case, n, mode=mode)\n        return module.metadata(case, n)")
        source = _once(source, '    descriptors = []\n',
                       "    fallback_requested = meta.get('table_service_overrides', meta.get('table_service_config', {})).get('EnableFallbackToYqlOptimizer', False)\n    mode_configuration = optimizer_mode_configuration(settings, mode, syntax, pragmas, fallback_requested)\n    descriptors = []\n")
        source = _once(source, '    try:\n        completed = subprocess.run(command,',
                       "    record.update(mode_configuration)\n    try:\n        if record['optimizer_mode_config_verified'] is not True:\n            record.update(harness_exception='OptimizerModeConfigurationMismatch', semantic_oracle_pass=False)\n            return record\n        completed = subprocess.run(command,")
        return source
    change('native_verify.py', native)
    def verdict(source):
        source = _once(source, 'import re\n', 'import re\n\nfrom optimizer_mode_contract import optimizer_mode_matches_record\n')
        source = _once(source, "    if record.get('native_batch_guard_pass') is False:",
                       "    if not optimizer_mode_matches_record(record):\n        return 'failure'\n    if record.get('native_batch_guard_pass') is False:")
        source = _once(source, "            success &= (record_verdict(left) == record_verdict(right) == 'functional'",
                       "            success &= (optimizer_mode_matches_record(left, require_exclusive=True)\n                        and optimizer_mode_matches_record(right, require_exclusive=True)\n                        and record_verdict(left) == record_verdict(right) == 'functional'")
        return source
    change('verification_contract.py', verdict)
    for name in ('interval_sum_checks.py', 'interval_window_checks.py'):
        def metadata(source):
            source = _once(source, 'def metadata(case, n):\n', "def metadata(case, n, mode='new'):\n")
            return _once(source, 'return dict(requirements(case), finding=', 'return dict(requirements(case, mode), finding=')
        change(name, metadata)
    return changes


def render_patch():
    return ''.join('diff --git a/' + name + ' b/' + name + '\n' +
                   ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + name, tofile='b/' + name))
                   for name, (before, after) in render_files().items())
