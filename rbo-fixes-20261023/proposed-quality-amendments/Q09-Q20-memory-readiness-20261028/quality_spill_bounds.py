"""Source-bound payload-file cap for the reviewed 82 quality SQL cells.

This adapter changes only the compute-spill mask and local-service byte cap.
It neither grants semantic success nor changes an optimization quality gate.
"""
import copy
import hashlib
import json
from pathlib import Path
from typing import NamedTuple

_ROOT = Path(__file__).resolve().parent.parent
VERSION = 'quality_sql_payload_spill_cap_v1'
LOCAL_SERVICE = '{ LocalFileConfig { Enable: true MaxTotalSize: 1 } }'

_BASE_REVISION = 'e8700d379b3b9c4fbaf68f36424e6bcf08314fd2'
_FIXED_REVISIONS = {'Q01': '3e2a57e559557a716bba6f42eccff44bbc2ed8d7',
 'Q03': '939173f5abcbcd596dd23ddfefed806df0d243ed',
 'Q04': 'a9df61306aa47badfbf1cec2fc043b3336addfd3',
 'Q06': '864bd56c1abb0c47f9f1ee669acf722819354dee',
 'Q07': '18eb64e0a7e18ccf2f50340a75f98b3ced6ab646',
 'Q10': 'efeebb38e72fd7a7c84ab65121e49362f93e2d4b',
 'Q14': '0600104a1fba93917e7a4e11a4f1b2ad0198cdbd',
 'Q15': '4e6d9217ef22bdcced5cd1879d3639508717e8e0',
 'Q17': 'a5a23189d946a8bd772e10279bc44e270648ec02',
 'Q21': 'a15cf405bd258b86dedf171e6a9eaac9022dd644',
 'Q23': '183be021902ede0113798fcee8118c4479966713',
 'Q24': '734379d38b24d68712098d96cd57fe79d1c90253',
 'Q28': '8089888107b179ca5c08f145f70564bd4637daa6',
 'Q29': '73620db7e22cbf67cb2e2262f227a9303492ed41'}

class _ValidatedSpillScope(NamedTuple):
    phase: str
    revision: str
    bugs: tuple
    runner: str
    receipt_sha256: str
    runner_sha256: str


def validated_quality_spill_scope(bugs, phase, revision, provenance):
    """Bind an already verified runner receipt to the audited production scope."""
    affected = tuple(bug for bug in bugs if bug in _FIXED_REVISIONS)
    if not affected:
        return None
    if (phase not in ('baseline', 'fixed')
            or any(revision != (_BASE_REVISION if phase == 'baseline' else _FIXED_REVISIONS[bug])
                   for bug in affected)
            or provenance.get('binary_provenance_verified') is not True
            or provenance.get('binary_provenance_version') != 'successful_runner_build_v1'):
        raise ValueError('Unaudited spilling production revision or runner provenance')
    raw = Path(provenance['build_receipt_path']).read_bytes()
    receipt_hash = hashlib.sha256(raw).hexdigest()
    receipt = json.loads(raw)
    binary = provenance.get('binary_provenance', {})
    saved_binary = receipt.get('binary', {})
    fields = {'resolved_path', 'sha256', 'size_bytes', 'mtime_ns', 'ctime_ns', 'inode', 'device'}
    if (receipt_hash != provenance.get('build_receipt_sha256')
            or receipt.get('schema_version') != 'successful_runner_build_v1'
            or receipt.get('revision') != revision
            or receipt.get('build', {}).get('exit_code') != 0
            or 'ydb/tests/tools/kqprun' not in receipt.get('build', {}).get('target', '').split()
            or set(binary) != fields or set(saved_binary) != fields
            or any(type(binary[key]) is not int or binary[key] < 0
                   for key in fields - {'resolved_path', 'sha256'})
            or type(saved_binary['ctime_ns']) is not int or saved_binary['ctime_ns'] < 0
            or binary['size_bytes'] <= 0
            or not isinstance(binary['sha256'], str) or len(binary['sha256']) != 64
            or any(c not in '0123456789abcdef' for c in binary['sha256'])
            or any(binary[key] != saved_binary[key] for key in fields - {'ctime_ns'})
            or not isinstance(binary['resolved_path'], str)
            or not Path(binary['resolved_path']).is_absolute()
            or not isinstance(receipt.get('worktree'), str)
            or not Path(receipt['worktree']).is_absolute()
            or (Path(receipt['worktree']) / 'ydb/tests/tools/kqprun/kqprun').resolve()
               != Path(binary['resolved_path'])):
        raise ValueError('Spilling runner receipt binding changed')
    return _ValidatedSpillScope(phase, revision, affected, binary['resolved_path'],
                                receipt_hash, binary['sha256'])


_SOURCE_BINDINGS = {'rbo-fixes-20261023/anonymous_cpp_guard.py': '58941801bad6b72f86546c669107e51cf2b765a806b1d8bb999ac4dfbf310819',
 'rbo-fixes-20261023/anonymous_cpp_metadata.py': '83c14f54667d8316a04361a166bedaf9da09a45c0b35322aee445eb1aa9dadbf',
 'rbo-fixes-20261023/anonymous_cpp_verify.py': 'f64988a4add5001d8226e5860ad82ce24c58e357b49de5327a01aa9054efd2c0',
 'rbo-fixes-20261023/correctness_cpp_contract.py': 'bd414815039456e1845193ff123fcc3be793a73059e4eee674ecb9cdb4bb435a',
 'rbo-fixes-20261023/memory_safe_build.py': 'ba2e6c524bfcf72a2d0507fceda8cd4877ce4a0570bdc1fbd0e56131933363bd',
 'rbo-fixes-20261023/optimizer_mode_contract.py': '50ffb02aff06f5b2322cf8b00c87500d24e73580c304120e48ffd76f329c75ad',
 'rbo-fixes-20261023/production_version_object.py': '55eb36ef4d02f70c7ef232e3cbf2411ce04166068862f42dfeeaa7bea50678a9',
 'rbo-fixes-20261023/quality_join_shuffle_checks.py': '2d817f9ba3fc4af3c0306910ce65a3b3640d5ecbe7ae3b14db7fdff6a10190cc',
 'rbo-fixes-20261023/quality_lookup_checks.py': '477596a7a71a3c65e8abdb0be218e4ae82c5dafd4414450d963798955cf16bae',
 'rbo-fixes-20261023/quality_minmax_checks.py': '42d8fbf1f983b739448dc1c6e19432ac464ea9f4a4bf6343e0a10e573f4d74d4',
 'rbo-fixes-20261023/quality_pairing.py': '622ec792339d50b26e6fcc0de7a8698caddcd179d20c1ec90c9dd7df08b052c5',
 'rbo-fixes-20261023/quality_partition_checks.py': '6e05ff277e5f75846efc58a6b61e837b86c55b54f4452a447a2caf7b0f84a5b7',
 'rbo-fixes-20261023/quality_partition_subset_checks.py': '4a7c8816c021df7388ec50fc873b4a76f9c61ed32d1b7a06b6e2fced88469f93',
 'rbo-fixes-20261023/quality_range_minmax_checks.py': '2d2a23f017e76a67552260d7e15dcfe1e2b53be727ac92543ab99b32978eb390',
 'rbo-fixes-20261023/quality_range_runtime_contract.py': 'f980ebb5397d8f4b8067a73b96a460fc754f7ade276e607c5215cd1f35cf7d6d',
 'rbo-fixes-20261023/quality_range_runtime_cpp_generator.py': 'a1b35a293147dca72d7b009845fbbe53ed8eaf490da5f43ab65d3db196b9fd75',
 'rbo-fixes-20261023/quality_rule_checks.py': '79928bb8b273843ea6c4dd575c11ea4aa8fe377741e5091d027193852eee236e',
 'rbo-fixes-20261023/quality_scheduler_semantic_checks.py': 'f98339c76ca2fc2c87cdef6e013fd09b726ed75d395c4ab60eaf19980bb975d7',
 'rbo-fixes-20261023/quality_source_settings_checks.py': 'a3ae2c3ac5a3b30359eb196660ab93e4037ce5d68a70f9d552f5252ed70be2ba',
 'rbo-fixes-20261023/quality_statistics_checks.py': '96540f6174e2921a6a8d5659fc163860c83766f020edcccc1cd30181edfed5d4',
 'rbo-fixes-20261023/quality_statistics_cpp_contract.py': '7522d964569b2af5b9336a1003196d7efb633cdd705b5205ff059363300fe44d',
 'rbo-fixes-20261023/quality_sum_checks.py': '0178be8fb6183a0fe1bfc9a058e0037dd918cf3ca6c96695fdcb7895584915fd',
 'rbo-fixes-20261023/quality_union_cpp_contract.py': '3b7bd8de5d199f08519718b34d27fe73e96db3b626209011e763d37f7920a8fb',
 'rbo-fixes-20261023/quality_window_checks.py': '6cbe9c4ad1d47f72f3e25016d41602c562f4b715f5ddd3d988c8d264a1bfe3ba',
 'rbo-fixes-20261023/verification_contract.py': '73a7608c2a8ea37395bd2336def4801a3aa77e607324376816fac996242cd952',
 'rbo-review-20261021/generate_and_verify.py': 'fa9f950e0d97e784574532f5092ee45ac8cabd8edfba91b9495bc81f8e9afaed',
 'rbo-review-20261021/legacy.conf': 'd8037a114c42f131af3ba9417fd81bb5bdd8a147b67fea3b6caff44bfecbd46a',
 'rbo-review-20261021/new.conf': '7496d5d173b6dc3a270f2ce7ca2faf78d3bd7864723c200d71e46d77bda7359d'}

_CELLS = {'q01_minmax_whole_control': {'metadata_sha256': 'ad3c598862ad39e0075f3dfe87eeaed5f0631c1bcc2bf0dab79fad080be5dc4e',
                              'module': 'quality_minmax_checks',
                              'nodes': 1},
 'q01_range_minmax_whole_asc': {'metadata_sha256': '2db185a0d0fc0c170b7340d653245099775fea331993c9c093497fdf093784ea',
                                'module': 'quality_range_minmax_checks',
                                'nodes': 1},
 'q01_range_minmax_whole_desc': {'metadata_sha256': '2db185a0d0fc0c170b7340d653245099775fea331993c9c093497fdf093784ea',
                                 'module': 'quality_range_minmax_checks',
                                 'nodes': 1},
 'q01_sum_whole_control': {'metadata_sha256': '26cd079c8e1a32b980b60d2d8206d05aee3558db4316d547426ac7043de4882f',
                           'module': 'quality_sum_checks',
                           'nodes': 1},
 'q03_covered_control': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                         'module': 'quality_lookup_checks',
                         'nodes': 1},
 'q03_disabled_control': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                          'module': 'quality_lookup_checks',
                          'nodes': 1},
 'q03_empty_result': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                      'module': 'quality_lookup_checks',
                      'nodes': 1},
 'q03_nullable_primary_key': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q03_uncovered_residual': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                            'module': 'quality_lookup_checks',
                            'nodes': 1},
 'q04_ascending': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                   'module': 'quality_lookup_checks',
                   'nodes': 1},
 'q04_descending': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                    'module': 'quality_lookup_checks',
                    'nodes': 1},
 'q04_direction_mismatch_control': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                                    'module': 'quality_lookup_checks',
                                    'nodes': 1},
 'q04_disabled_control': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                          'module': 'quality_lookup_checks',
                          'nodes': 1},
 'q04_nonmatching_index_control': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                                   'module': 'quality_lookup_checks',
                                   'nodes': 1},
 'q04_nullable_order': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                        'module': 'quality_lookup_checks',
                        'nodes': 1},
 'q04_unbounded_control': {'metadata_sha256': '6d59485d6f2e9c9ab8f635af0178b2e8d2ec00eaff18c9a0d0f61889370c494c',
                           'module': 'quality_lookup_checks',
                           'nodes': 1},
 'q06_complete_primary_key': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q06_disabled_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                          'module': 'quality_lookup_checks',
                          'nodes': 1},
 'q06_nullable_key': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                      'module': 'quality_lookup_checks',
                      'nodes': 1},
 'q06_partial_primary_control': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                                 'module': 'quality_lookup_checks',
                                 'nodes': 1},
 'q07_automatic_mixed': {'metadata_sha256': '5df259283d1f32c36b00d0adc01e3726d6276f8bb8e88df9bb630d3f1fea9bb0',
                         'module': 'quality_lookup_checks',
                         'nodes': 1},
 'q07_disabled_lookup': {'metadata_sha256': '8419909f12d523d9c560c47d5daa7c4637246e018ca8e21433f8fab4ba1b1100',
                         'module': 'quality_lookup_checks',
                         'nodes': 1},
 'q07_disabled_lookup_large_right': {'metadata_sha256': 'ab8839d6413d0d8e91d4dfd8f0982a53cca072caf24ee70c496054fd736d224d',
                                     'module': 'quality_lookup_checks',
                                     'nodes': 4},
 'q07_exact_key_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                           'module': 'quality_lookup_checks',
                           'nodes': 1},
 'q07_left_outer_mixed': {'metadata_sha256': '4ccc1a9879725b8d365602eac8ae68b98b54a31690d16f96881ad34ef5fa5180',
                          'module': 'quality_lookup_checks',
                          'nodes': 4},
 'q07_mixed_key': {'metadata_sha256': '4ccc1a9879725b8d365602eac8ae68b98b54a31690d16f96881ad34ef5fa5180',
                   'module': 'quality_lookup_checks',
                   'nodes': 4},
 'q07_mixed_key_large_right': {'metadata_sha256': '59f5e12a7de27e03f8011043d6aee3a833855c2ad5cf8ac7c767053838cf08de',
                               'module': 'quality_lookup_checks',
                               'nodes': 4},
 'q07_nested_fallback': {'metadata_sha256': '4ccc1a9879725b8d365602eac8ae68b98b54a31690d16f96881ad34ef5fa5180',
                         'module': 'quality_lookup_checks',
                         'nodes': 4},
 'q07_null_safe': {'metadata_sha256': 'c5698e758c21af5b0d4309a23ec4097a8c04868a12eb5071ea2580d424e349d9',
                   'module': 'quality_lookup_checks',
                   'nodes': 1},
 'q07_optional_key_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q07_residual_filter': {'metadata_sha256': 'c5698e758c21af5b0d4309a23ec4097a8c04868a12eb5071ea2580d424e349d9',
                         'module': 'quality_lookup_checks',
                         'nodes': 1},
 'q10_grouped_all_null': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                          'module': 'quality_window_checks',
                          'nodes': 1},
 'q10_grouped_different': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                           'module': 'quality_window_checks',
                           'nodes': 1},
 'q10_grouped_empty': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                       'module': 'quality_window_checks',
                       'nodes': 1},
 'q10_grouped_ordinary': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                          'module': 'quality_window_checks',
                          'nodes': 1},
 'q10_grouped_shared': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                        'module': 'quality_window_checks',
                        'nodes': 1},
 'q10_scalar_all_null': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                         'module': 'quality_window_checks',
                         'nodes': 1},
 'q10_scalar_different': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                          'module': 'quality_window_checks',
                          'nodes': 1},
 'q10_scalar_empty': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                      'module': 'quality_window_checks',
                      'nodes': 1},
 'q10_scalar_ordinary': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                         'module': 'quality_window_checks',
                         'nodes': 1},
 'q10_scalar_shared': {'metadata_sha256': 'd0d33ef3281fee8b8da73ecc8f2dac739a190de47e63cc403c84aad2d848c112',
                       'module': 'quality_window_checks',
                       'nodes': 1},
 'q14_grace_chain_off': {'metadata_sha256': 'ae76daf9e2aaf31e0e8bff0159bfd3d11dbbed627c1e1e430ada0d883c80270c',
                         'module': 'quality_join_shuffle_checks',
                         'nodes': 4},
 'q14_grace_chain_on': {'metadata_sha256': 'f59b9b17efd87d1d8f24af21f17e9c2ef554dd2d55c59c4cb405ca64481935fe',
                        'module': 'quality_join_shuffle_checks',
                        'nodes': 4},
 'q14_reverse_chain_off': {'metadata_sha256': 'cb6b572db5e96106387a61d20288299a5c4e6d0902de3a4aecbab8fc990b60da',
                           'module': 'quality_join_shuffle_checks',
                           'nodes': 4},
 'q14_reverse_chain_on': {'metadata_sha256': '9473024eec1f6dfa8e5a9acf91c6eae077a09bb3050b8560de7b6bc3d9f6e486',
                          'module': 'quality_join_shuffle_checks',
                          'nodes': 4},
 'q14_reverse_single_off_control': {'metadata_sha256': '4c87d7ae85ab48b34a11069a5060fd66c8201f774d41ec339e716e0562cd9b9b',
                                    'module': 'quality_join_shuffle_checks',
                                    'nodes': 4},
 'q14_reverse_single_on_control': {'metadata_sha256': '6ce8c083e42ab8097374d21fac7605ee55a7809f1d78eb60eb75dfe37c11e4f9',
                                   'module': 'quality_join_shuffle_checks',
                                   'nodes': 4},
 'q15_after_aggregate': {'metadata_sha256': '08593a5ca84d0dcb7e0e22ebfb0851ba34f01b367d914eff12148294f6b45183',
                         'module': 'quality_partition_subset_checks',
                         'nodes': 4},
 'q17_declared_cover_control': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                                'module': 'quality_lookup_checks',
                                'nodes': 1},
 'q17_disabled_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                          'module': 'quality_lookup_checks',
                          'nodes': 1},
 'q17_implicit_primary_key': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q17_nullable_primary_key': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q17_uncovered_control': {'metadata_sha256': 'eb074f8882cf1d23b3fbca7a45413d564468447c54262a8191939226416e08bc',
                           'module': 'quality_lookup_checks',
                           'nodes': 1},
 'q21_exists_filter_1': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                         'module': 'quality_rule_checks',
                         'nodes': 1},
 'q21_exists_filter_3': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                         'module': 'quality_rule_checks',
                         'nodes': 1},
 'q21_exists_generic_1': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                          'module': 'quality_rule_checks',
                          'nodes': 1},
 'q21_exists_generic_3': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                          'module': 'quality_rule_checks',
                          'nodes': 1},
 'q21_in_control': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                    'module': 'quality_rule_checks',
                    'nodes': 1},
 'q21_not_exists_filter_1': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                             'module': 'quality_rule_checks',
                             'nodes': 1},
 'q21_not_exists_filter_3': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                             'module': 'quality_rule_checks',
                             'nodes': 1},
 'q21_not_exists_generic_1': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                              'module': 'quality_rule_checks',
                              'nodes': 1},
 'q21_not_exists_generic_3': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                              'module': 'quality_rule_checks',
                              'nodes': 1},
 'q21_offset_control': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                        'module': 'quality_rule_checks',
                        'nodes': 1},
 'q21_scalar_control': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                        'module': 'quality_rule_checks',
                        'nodes': 1},
 'q21_unlimited_control': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                           'module': 'quality_rule_checks',
                           'nodes': 1},
 'q21_zero_control': {'metadata_sha256': '253a4abf4483c72a69faf5134941826d619711bbefd03e0d259c46f5b616edf4',
                      'module': 'quality_rule_checks',
                      'nodes': 1},
 'q23_dead_probe_payload': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                            'module': 'quality_lookup_checks',
                            'nodes': 1},
 'q23_left_outer': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                    'module': 'quality_lookup_checks',
                    'nodes': 1},
 'q23_prefix_equality': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                         'module': 'quality_lookup_checks',
                         'nodes': 1},
 'q23_residual_key_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q23_selected_probe_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                                'module': 'quality_lookup_checks',
                                'nodes': 1},
 'q24_duplicate_alias_control': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                                 'module': 'quality_lookup_checks',
                                 'nodes': 1},
 'q24_empty_payload': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                       'module': 'quality_lookup_checks',
                       'nodes': 1},
 'q24_range_only_field': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                          'module': 'quality_lookup_checks',
                          'nodes': 1},
 'q24_residual_field_control': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                                'module': 'quality_lookup_checks',
                                'nodes': 1},
 'q24_selected_key_control': {'metadata_sha256': '0ba9e764e0846868c5e4fc82893c603946c2221981e2cda3db874ba54bc535e5',
                              'module': 'quality_lookup_checks',
                              'nodes': 1},
 'q28_shared_spilling_control': {'metadata_sha256': '1b11ff8015d2b287fe77bb6dd9e4c9dff57a65e77c53df79df3d2313d0e318ff',
                                 'module': 'quality_scheduler_semantic_checks',
                                 'nodes': 1},
 'q29_arithmetic_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                            'module': 'quality_lookup_checks',
                            'nodes': 1},
 'q29_guard_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                       'module': 'quality_lookup_checks',
                       'nodes': 1},
 'q29_no_range_control': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                          'module': 'quality_lookup_checks',
                          'nodes': 1},
 'q29_optional_residual': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                           'module': 'quality_lookup_checks',
                           'nodes': 1},
 'q29_primitive_residual': {'metadata_sha256': '15d7818ce4958e649cbb9dcbd8b31ebd8aa8c67dcd9452893b29c23995acdbe7',
                            'module': 'quality_lookup_checks',
                            'nodes': 1}}


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def bound_quality_spill_metadata(module, case, mode, rows, metadata, *, scope=None, runner=None):
    settings = metadata.get('table_service_overrides', metadata.get('table_service_config', {}))
    node_mask = settings.get('EnableSpillingNodes', 'None')
    enabled = (settings.get('EnableQueryServiceSpilling')
               or settings.get('EnableSpillingInHashJoinShuffleConnections')
               or node_mask not in ('None', '"None"', None, 0, False))
    entry = _CELLS.get(case)
    if entry is None:
        if case.startswith('q') and enabled:
            raise ValueError('Unreviewed spilling quality profile')
        return metadata
    bug = case.split('_', 1)[0].upper()
    if (not isinstance(scope, _ValidatedSpillScope) or bug not in scope.bugs
            or scope.phase not in ('baseline', 'fixed')
            or scope.revision != (_BASE_REVISION if scope.phase == 'baseline' else _FIXED_REVISIONS[bug])
            or runner is None or Path(runner).resolve() != Path(scope.runner)):
        raise ValueError('Spilling cell requires a validated audited runner scope')
    if (mode != 'new' or type(rows) is not int or rows != 24
            or module.__name__ != entry['module']
            or _digest(metadata) != entry['metadata_sha256']
            or settings.get('EnableQueryServiceSpilling') is not True
            or settings.get('EnableSpillingInHashJoinShuffleConnections') is not True
            or settings.get('EnableSpillingNodes') != 'All'
            or 'SpillingServiceConfig' in settings):
        raise ValueError('Spilling quality profile changed or out of reviewed scope')
    expected_path = _ROOT / 'rbo-fixes-20261023' / (entry['module'] + '.py')
    expected_hash = _SOURCE_BINDINGS[str(expected_path.relative_to(_ROOT))]
    if (Path(module.__file__).resolve() != expected_path
            or getattr(module, '_verification_source_sha256', None) != expected_hash):
        raise ValueError('Spilling generator source snapshot is not reviewed')
    for name, expected in _SOURCE_BINDINGS.items():
        if hashlib.sha256((_ROOT / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Spilling generator dependency changed')
    result = copy.deepcopy(metadata)
    result['table_service_overrides'].update(
        EnableSpillingNodes='None', SpillingServiceConfig=LOCAL_SERVICE)
    result['spill_payload_file_policy'] = {
        'version': VERSION, 'local_service_enabled': True, 'cap_bytes': 1,
        'compute_spill_mask': 'None', 'source_bindings_sha256': _digest(_SOURCE_BINDINGS),
        'source_metadata_sha256': entry['metadata_sha256'],
        'production_phase': scope.phase, 'production_revision': scope.revision,
        'build_receipt_sha256': scope.receipt_sha256, 'runner_sha256': scope.runner_sha256,
        'runtime_spill_occurrence_observed': False,
        'cap_rejection_counts_as_success': False,
    }
    return result
