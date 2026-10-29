"""Compose the reviewed C47 copy and covering vectors; render only in memory."""
import hashlib
import json
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parent
VERSION = 'c47_combined_production_contracts_v1'
BINDINGS = [{'vector': 'copy', 'module': 'c47_lookup_copy_contract_generator', 'generator_sha256': '7e717869cb10b65043ade9032a170c3dfec96e8e341847c774245637f35033df', 'rendered_source_sha256': 'e610ac774afe761407c33e1565ea9a74d93d5d6a7429e2fc0572dda8326a9d4c', 'case_count': 19}, {'vector': 'covering', 'module': 'c47_covering_import_contract_generator', 'generator_sha256': 'd19a363c250f6f61702e29360b50b613dbc42bf0f0b19c0eab3f5a39c608cee6', 'rendered_source_sha256': '83fe696c750b874b7addcb51971cf5f3814c523bacfc115e2571d07173d78483', 'case_count': 32}]
COPY_ASSERTIONS = [17, 18, 25, 26, 17, 18, 25, 26, 25, 27, 3, 3, 3, 3, 3, 3, 3, 2, 2]


def _load(binding):
    path = ROOT / (binding['module'] + '.py')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != binding['generator_sha256']:
        raise ValueError('C47 component generator requires renewed review')
    module = ModuleType(binding['module'])
    module.__file__ = str(path)
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    rendered = module.render_cpp_suite()
    if (len(module.cases()) != binding['case_count']
        or hashlib.sha256(rendered.encode()).hexdigest() != binding['rendered_source_sha256']):
        raise ValueError('C47 component contract differs from reviewed source')
    return module, rendered


def _components():
    return [(binding, *_load(binding)) for binding in BINDINGS]


def cases():
    return [binding['vector'] + '/' + case
            for binding, module, _ in _components() for case in module.cases()]


def specs():
    result = []
    for binding, module, _ in _components():
        counts = (COPY_ASSERTIONS if binding['vector'] == 'copy'
                  else [item['assertion_count'] for item in module.specs()])
        if len(counts) != len(module.cases()):
            raise ValueError('C47 case/assertion vector differs')
        for case, count in zip(module.cases(), counts):
            result.append({'case_index': len(result), 'vector': binding['vector'],
                           'case': case, 'assertion_count': count})
    return result


def vector_manifest():
    result, first = [], 0
    for binding, _, _ in _components():
        result.append(dict(binding, first_case_index=first))
        first += binding['case_count']
    return result


def render_cpp_suite():
    # The component functions and their helpers stay byte-identical. Only their
    # file-scope includes and standalone mains are replaced by one driver.
    includes, groups, calls = [], [], []
    for binding, module, rendered in _components():
        marker = 'int main() { unsigned failures = 0;\n'
        if rendered.count(marker) != 1:
            raise ValueError('C47 component main boundary changed')
        prefix, _ = rendered.split(marker)
        body = []
        for line in prefix.splitlines(True):
            if line.startswith('#include <') and line.endswith('>\n'):
                if line not in includes:
                    includes.append(line)
            elif line.startswith('#'):
                raise ValueError('C47 component preprocessing requires renewed review')
            else:
                body.append(line)
        namespace = 'C47_' + binding['vector']
        groups.append('namespace ' + namespace + ' {\n' + ''.join(body) + '}\n')
        calls.extend((namespace, index) for index in range(len(module.cases())))
    source = ''.join(includes + groups) + 'int main() { unsigned failures = 0;\n'
    for index, (namespace, local_index) in enumerate(calls):
        source += '{ const auto before = ' + namespace + '::Checks; bool passed = true; try { '
        source += namespace + '::Case' + str(local_index) + '(); } catch (...) { passed = false; ++failures; }\n'
        source += 'std::cout << ' + json.dumps('{"case_index":' + str(index) + ',"checks_executed":')
        source += ' << (' + namespace + '::Checks-before) << ' + json.dumps(',"passed":')
        source += ' << (passed ? "true" : "false") << ' + json.dumps('}\n') + '; }\n'
    return source + 'return failures ? 1 : 0; }\n'
