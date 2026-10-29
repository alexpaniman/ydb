"""Sanitized requested-optimizer configuration evidence; no runtime claims."""
import hashlib
import re

VERSION = 'requested_optimizer_configuration_v1'


def optimizer_mode_configuration(settings, mode, syntax, pragmas, fallback_requested=False):
    def flag(name):
        values = re.findall(r'(?m)^\s*' + name + r':[^\S\n]*([^\n]*)$', settings)
        values = [value.strip() for value in values]
        return values[0] == 'true' if len(values) == 1 and values[0] in ('true', 'false') else None
    enabled = flag('EnableNewRBO')
    fallback = flag('EnableFallbackToYqlOptimizer')
    yql_select = pragmas.get('YqlSelect') if syntax == 'yql' else None
    configuration = dict(version=VERSION, requested_mode=mode, query_syntax=syntax,
                         enable_new_rbo=enabled, enable_fallback=fallback,
                         fallback_requested=fallback_requested,
                         yql_select=yql_select, settings_sha256=hashlib.sha256(settings.encode()).hexdigest())
    verified = (mode in ('new', 'legacy') and type(enabled) is bool
                and enabled is (mode == 'new') and type(fallback_requested) is bool
                and fallback is fallback_requested
                and (syntax == 'yql' and yql_select == ('force' if mode == 'new' else 'disable')
                     or syntax == 'pg' and yql_select is None))
    return dict(optimizer_mode_configuration=configuration, optimizer_mode_config_verified=verified)


def optimizer_mode_matches_record(record, require_exclusive=False):
    """Exclusive comparisons require complete proof and disabled fallback."""
    if not require_exclusive and 'optimizer_mode_config_verified' not in record and 'optimizer_mode_configuration' not in record:
        return True  # Historical records retain their existing, narrower verdict.
    config = record.get('optimizer_mode_configuration')
    mode, syntax = record.get('mode'), record.get('query_syntax')
    return (record.get('optimizer_mode_config_verified') is True and isinstance(config, dict)
            and config.get('version') == VERSION and mode in ('new', 'legacy')
            and config.get('requested_mode') == mode and config.get('query_syntax') == syntax
            and type(config.get('enable_new_rbo')) is bool and config['enable_new_rbo'] is (mode == 'new')
            and type(config.get('fallback_requested')) is bool
            and config.get('enable_fallback') is config['fallback_requested']
            and (not require_exclusive or config['enable_fallback'] is False)
            and isinstance(record.get('settings_sha256'), str)
            and re.fullmatch('[0-9a-f]{64}', record['settings_sha256']) is not None
            and config.get('settings_sha256') == record['settings_sha256']
            and (syntax == 'yql' and config.get('yql_select') == ('force' if mode == 'new' else 'disable')
                 or syntax == 'pg' and config.get('yql_select') is None))
