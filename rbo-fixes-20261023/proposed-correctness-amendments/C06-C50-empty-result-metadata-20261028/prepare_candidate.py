"""Prepare narrow integration-only empty-result metadata transport candidates."""
import difflib
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOKEN = 'c06_c50_empty_result_metadata_v1'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('Candidate source anchor changed')
    return source.replace(before, after, 1)


def main():
    files = ('native_verify.py', 'c06_c50_bounded_integration_checks.py', 'c06_c50_integration_runner.py')
    before = {name: (ROOT / name).read_text() for name in files}
    native = before['native_verify.py']
    native = replace_once(native,
        "    query = module.recipes(n)[case]\n",
        "    empty_result_witness = meta.get('c06_c50_empty_result_witness')\n"
        "    if empty_result_witness is not None:\n"
        "        if (empty_result_witness != 'c06_c50_empty_result_metadata_v1'\n"
        "            or module.__name__ != 'c06_c50_bounded_integration_checks'\n"
        "            or case not in ('c50_left_anti', 'c50_left_anti_same_type_control')\n"
        "            or mode != 'new' or type(n) is not int or n != 24\n"
        "            or not getattr(module, '_verification_source_sha256', None)):\n"
        "            raise ValueError('Unreviewed integration empty-result witness request')\n"
        "    query = module.recipes(n)[case]\n")
    native = replace_once(native,
        "    if validations and not hasattr(module, 'check_validation_results'):\n",
        "    if empty_result_witness is not None and validations:\n"
        "        raise ValueError('Empty-result metadata witness requires the original single target')\n"
        "    if validations and not hasattr(module, 'check_validation_results'):\n")
    native = replace_once(native,
        "    command += ['-C', 'query', '--log-file', '/dev/null', '--result-file', memory('results', '')]\n",
        "    command += ['-C', 'query', '--log-file', '/dev/null', '--result-file', memory('results', '')]\n"
        "    if empty_result_witness is not None:\n"
        "        command += ['--result-format', 'full-json']\n")
    native = replace_once(native,
        "    if 'spill_payload_file_policy' in meta:\n",
        "    if empty_result_witness is not None:\n"
        "        record['result_output_format'] = 'full-json'\n"
        "    if 'spill_payload_file_policy' in meta:\n")
    native = replace_once(native,
        "            rows, result_sets, boundaries_known = parse_rows_output(payload)\n",
        "            if empty_result_witness is not None:\n"
        "                rows, result_sets, boundaries_known, witness = module.parse_empty_result_witness(case, payload, n)\n"
        "                record.update(witness)\n"
        "            else:\n"
        "                rows, result_sets, boundaries_known = parse_rows_output(payload)\n")
    wrapper = before['c06_c50_bounded_integration_checks.py']
    wrapper = replace_once(wrapper, "VERSION = 'c06_c50_bounded_integration_v1'",
        "VERSION = 'c06_c50_bounded_integration_empty_metadata_v2'\n"
        "EMPTY_WITNESS_VERSION = 'c06_c50_empty_result_metadata_v1'\n"
        "EMPTY_WITNESS_CASES = ('c50_left_anti', 'c50_left_anti_same_type_control')")
    wrapper = replace_once(wrapper, "    value['generator_version'] = VERSION\n    return value\n",
        "    value['generator_version'] = VERSION\n"
        "    if case in EMPTY_WITNESS_CASES:\n"
        "        value['c06_c50_empty_result_witness'] = EMPTY_WITNESS_VERSION\n"
        "    return value\n")
    parser = '''
def parse_empty_result_witness(case, payload, n):
    """Require one actual full-json ResultSet, never infer a set from no bytes."""
    original = _cell(case, n)
    if (case not in EMPTY_WITNESS_CASES or not isinstance(payload, bytes)
        or not 0 < len(payload) <= 16 * 1024):
        raise ValueError('Unreviewed empty-result metadata witness')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate empty-result metadata key')
            result[key] = value
        return result
    value = json.loads(payload, object_pairs_hook=unique)
    # kqprun full-json calls ResultSet.PrintJSON. Its protobuf generator omits
    # empty repeated fields and default scalar fields; nonempty rows, truncated
    # output, Arrow data and unknown fields must never pass this narrow witness.
    if (not isinstance(value, dict) or set(value) not in ({'columns'}, {'columns', 'format'})
        or value.get('columns') != [{'name': 'li', 'type': {'type_id': 3}}]
        or type(value['columns'][0]['type']['type_id']) is not int
        or ('format' in value and (type(value['format']) is not int or value['format'] != 1))
        or original.expected_rows(case, n) != []):
        raise ValueError('Empty-result metadata does not prove the expected result set')
    return [], [[]], True, {
        'empty_result_set_witness_version': EMPTY_WITNESS_VERSION,
        'empty_result_set_metadata_verified': True,
        'empty_result_set_payload_sha256': _hash(payload),
    }


'''
    wrapper = replace_once(wrapper, 'def required_record_vector():\n', parser + 'def required_record_vector():\n')
    wrapper = replace_once(wrapper,
        "    # A failing semantic verdict remains a failing record, never an exception\n",
        "    witness_fields = ('empty_result_set_witness_version', 'empty_result_set_metadata_verified',\n"
        "                      'empty_result_set_payload_sha256', 'result_output_format')\n"
        "    if record['case'] in EMPTY_WITNESS_CASES:\n"
        "        if (record.get('empty_result_set_witness_version') != EMPTY_WITNESS_VERSION\n"
        "            or record.get('empty_result_set_metadata_verified') is not True\n"
        "            or record.get('result_output_format') != 'full-json'\n"
        "            or not isinstance(record.get('empty_result_set_payload_sha256'), str)\n"
        "            or len(record['empty_result_set_payload_sha256']) != 64\n"
        "            or any(char not in '0123456789abcdef' for char in record['empty_result_set_payload_sha256'])\n"
        "            or record.get('empty_result_set_payload_sha256') != record.get('result_sha256')\n"
        "            or type(record.get('row_count')) is not int or record['row_count'] != 0\n"
        "            or type(record.get('result_set_count')) is not int or record['result_set_count'] != 1):\n"
        "            raise ValueError('Missing bound empty-result metadata witness')\n"
        "    elif any(field in record for field in witness_fields):\n"
        "        raise ValueError('Empty-result witness used outside its reviewed cases')\n"
        "    # A failing semantic verdict remains a failing record, never an exception\n")
    wrapper = replace_once(wrapper, sha(before['native_verify.py'].encode()), sha(native.encode()))
    runner = before['c06_c50_integration_runner.py']
    runner = replace_once(runner, "VERSION = 'c06_c50_guarded_integration_runner_v1'",
                          "VERSION = 'c06_c50_guarded_integration_empty_metadata_v2'")
    runner = replace_once(runner, "    'harness_exception','failure_category','failure_stage','diagnostic_sha256','diagnostic_bytes'))",
        "    'harness_exception','failure_category','failure_stage','diagnostic_sha256','diagnostic_bytes',\n"
        "    'result_output_format','empty_result_set_witness_version','empty_result_set_metadata_verified',\n"
        "    'empty_result_set_payload_sha256'))")
    runner = replace_once(runner, sha(before['native_verify.py'].encode()), sha(native.encode()))
    runner = replace_once(runner, sha(before['c06_c50_bounded_integration_checks.py'].encode()), sha(wrapper.encode()))
    after = dict(zip(files, (native, wrapper, runner)))
    patch = ROOT / 'proposed-correctness-amendments/C06-C50-empty-result-metadata-20261028.patch'
    pieces = []
    for name in files:
        (HERE / name).write_text(after[name])
        target = ROOT.name + '/' + name
        pieces.append('diff --git a/' + target + ' b/' + target + '\n' + ''.join(difflib.unified_diff(
            before[name].splitlines(True), after[name].splitlines(True), fromfile='a/' + target, tofile='b/' + target)))
    patch.write_text(''.join(pieces))
    manifest = {'schema': 'c06_c50_empty_result_transport_candidate_v1',
        'before': {name: sha(text.encode()) for name, text in before.items()},
        'after': {name: sha(text.encode()) for name, text in after.items()},
        'patch': str(patch.relative_to(ROOT)), 'patch_sha256': sha(patch.read_bytes())}
    (HERE / 'source-images.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
