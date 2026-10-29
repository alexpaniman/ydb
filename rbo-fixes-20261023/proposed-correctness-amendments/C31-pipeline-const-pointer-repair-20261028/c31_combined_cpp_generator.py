"""Compose unchanged C31 identity contracts and four actual pipeline checkpoints."""
import hashlib
import json
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parent
BINDINGS = [{'vector': 'identity', 'module': 'c31_identity_cpp_generator', 'generator_sha256': '112ca72315a8807599d960fe98e80d3b1128b37be9967ad17ff934f02370b8f9', 'rendered_source_sha256': '59fb5d9eea7fd6890d7d75f12616f62551efc4b3d587ca0bc2bf8ff0a8395a0d', 'case_count': 33, 'main_boundary': 'int main() { unsigned failures=0;\n', 'function_prefix': 'Run'}, {'vector': 'pipeline', 'module': 'c31_shared_pipeline_cpp_generator', 'generator_sha256': '7237a294c0a31eb9da9ed6cd8069b643fa00dd38690ace3ec031d13938c2c4d6', 'rendered_source_sha256': '550d49960e5e34ac9edac3c08afc01a908418b67d42816438b75616735aa7992', 'case_count': 4, 'main_boundary': 'int main() { unsigned failures = 0;\n', 'function_prefix': 'Case'}]
IDENTITY_ASSERTIONS = [51, 51, 51, 66, 66, 66, 61, 61, 61, 48, 48, 48, 4, 4, 4, 3, 3, 3, 4, 4, 4, 59, 59, 59, 55, 55, 55, 55, 55, 55, 66, 66, 66]


def _components():
    result = []
    for binding in BINDINGS:
        path = ROOT / (binding['module'] + '.py')
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != binding['generator_sha256']:
            raise ValueError('C31 component source requires renewed review')
        module = ModuleType(binding['module'])
        module.__file__ = str(path)
        exec(compile(raw, str(path), 'exec'), module.__dict__)
        source = module.render_cpp_suite()
        if (len(module.cases()) != binding['case_count']
            or hashlib.sha256(source.encode()).hexdigest() != binding['rendered_source_sha256']):
            raise ValueError('C31 component render differs from review')
        result.append((binding, module, source))
    return result


def cases():
    return [binding['vector'] + '/' + case
            for binding, module, _ in _components() for case in module.cases()]


def specs():
    result = []
    for binding, module, _ in _components():
        counts = IDENTITY_ASSERTIONS if binding['vector'] == 'identity' else [x['assertion_count'] for x in module.specs()]
        if len(counts) != len(module.cases()):
            raise ValueError('C31 case/assertion vector changed')
        for case, count in zip(module.cases(), counts):
            result.append({'case_index': len(result), 'vector': binding['vector'],
                           'case': case, 'assertion_count': count})
    return result


def vector_manifest():
    result, first = [], 0
    for binding, _, _ in _components():
        row = {k: v for k, v in binding.items() if k not in ('main_boundary', 'function_prefix')}
        result.append(dict(row, first_case_index=first))
        first += binding['case_count']
    return result


def render_cpp_suite():
    includes, groups, calls = [], [], []
    for binding, module, source in _components():
        marker = binding['main_boundary']
        if source.count(marker) != 1:
            raise ValueError('C31 component main boundary changed')
        prefix, _ = source.split(marker)
        body = []
        for line in prefix.splitlines(True):
            if line.startswith('#include <') and line.endswith('>\n'):
                if line not in includes:
                    includes.append(line)
            elif line.startswith('#'):
                raise ValueError('C31 component preprocessing requires review')
            else:
                body.append(line)
        namespace = 'C31_' + binding['vector']
        groups.append('namespace ' + namespace + ' {\n' + ''.join(body) + '}\n')
        calls.extend((namespace, binding['function_prefix'] + str(i)) for i in range(len(module.cases())))
    text = ''.join(includes + groups) + 'int main() { unsigned failures = 0;\n'
    for i, (namespace, function) in enumerate(calls):
        text += '{ const auto before = ' + namespace + '::Checks; bool passed = true; try { '
        text += namespace + '::' + function + '(); } catch (...) { passed = false; ++failures; }\n'
        text += 'std::cout << ' + json.dumps('{"case_index":' + str(i) + ',"checks_executed":')
        text += ' << (' + namespace + '::Checks-before) << ' + json.dumps(',"passed":')
        text += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    return text + 'return failures ? 1 : 0; }\n'
