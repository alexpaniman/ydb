"""Eight fixed integration controls with a reviewed local payload-file bound.

A dedicated serial guarded runner must initialize this module with a fresh
successful executable receipt for the exact combined tree before requesting
any workload. This module never starts a process other than read-only Git.
Generated queries, schemas, fixtures and rows stay in caller memory.
"""
import builtins
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
from types import ModuleType, SimpleNamespace

_ROOT = Path(__file__).resolve().parent
_PROJECT = _ROOT.parent
VERSION = 'c06_c50_bounded_integration_empty_metadata_v2'
EMPTY_WITNESS_VERSION = 'c06_c50_empty_result_metadata_v1'
EMPTY_WITNESS_CASES = ('c50_left_anti', 'c50_left_anti_same_type_control')
CASE_SPECIFIC_SCHEMA = True
LOCAL_SERVICE = '{ LocalFileConfig { Enable: true MaxTotalSize: 1 } }'
EXPECTED_TREE = '138b36fd19ebf6d44f37aa3c73585e9c8be6ff5d'
# Frozen by the source-preparation tool; these bind loaded bytes, not import caches.
SOURCE_BINDINGS = {'rbo-fixes-20261023/join_key_checks.py': '32c91d788fd03cbb7035ebfea624f0b62eb012992810944c7450b509020a1acb', 'rbo-fixes-20261023/window_checks.py': 'c48fc8c06ddc561788de2c28d578b794390649a1b323b87ddd3e11f9af66698e', 'rbo-review-20261021/generate_and_verify.py': 'fa9f950e0d97e784574532f5092ee45ac8cabd8edfba91b9495bc81f8e9afaed', 'rbo-review-20261021/new.conf': '7496d5d173b6dc3a270f2ce7ca2faf78d3bd7864723c200d71e46d77bda7359d', 'rbo-fixes-20261023/native_verify.py': '0ddf84d7d8424a580922a63ac6d1dcaae27ff065fd44149dd88d29c294f9fc77', 'rbo-fixes-20261023/binary_provenance.py': 'b738e1aefd321965d62009f66739014ab45331ce8d1816ff59a1195e3f98451c', 'rbo-fixes-20261023/optimizer_mode_contract.py': '50ffb02aff06f5b2322cf8b00c87500d24e73580c304120e48ffd76f329c75ad', 'rbo-fixes-20261023/verification_contract.py': '73a7608c2a8ea37395bd2336def4801a3aa77e607324376816fac996242cd952', 'rbo-fixes-20261023/anonymous_cpp_guard.py': '58941801bad6b72f86546c669107e51cf2b765a806b1d8bb999ac4dfbf310819', 'rbo-fixes-20261023/verify_series.py': 'cc07fc271efde3ed8f8377ef86f9560b484591503d7c0198d89e7ca7488d6265', 'rbo-fixes-20261023/verification/C06-C50-integration-source-preparation-20261028.json': '17473d3cb4501754409d61c1ca225f147a8a1041f0ff0a55f752ba52832d0fa5', 'rbo-fixes-20261023/verification/C06-C50-integration-independent-source-review-20261028.json': '530fb5c69460669e57836114a016ab5c9924476095308cf5ba867784c2698516', 'rbo-fixes-20261023/verification/C06-C50-bounded-spill-production-source-comparison-20261028.json': '560c8365e4c66f6039d953f72d2bb1be2c147db13e165b1c6bbe0c89b746f514', 'rbo-fixes-20261023/verification/guarded-fixed-C50-946575513d23-1793145911575205063.jsonl': 'ee458a3f9112bc998787db65e5a0816b265d1a979a7ff8fb5b0dd9448033316f', 'rbo-fixes-20261023/quality_spill_bounds.py': 'dface8b7323a5724760d2299cbae1c8276f83768b2b9692d0435998d0cc4360a'}
PRODUCTION_BINDINGS = {'util/ysaveload.h': '1274e6043f22a0504fbf66f5e7e476718d6dcf515a9f83ee6ebe0db32227bf64', 'ydb/core/kqp/compute_actor/kqp_compute_actor_factory.cpp': '773ce794635c757808a0b67c00b7d5f21655c9d5b4ac888ff082ef663da0a4f1', 'ydb/core/kqp/compute_actor/kqp_compute_actor_impl.h': '89c4993237e176e7c9681c523d654e856d5bd7e2ce02702cf3926552f3f63b35', 'ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp': 'd93d2f3b9c5bbf9a66d4233c1d2b0c9d879be07dadae8e1751a2ea39471aecaf', 'ydb/core/kqp/opt/rbo/kqp_rbo_transformer.cpp': '593b73e3bfe99b67f9957cb4fe00da320ee0262af75c830929b50b42b8290896', 'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_aggregation_builder.cpp': 'fdb9b801ed168150a67bbb69ec53a83d969a8231cff4a0af52e37983d9c09f15', 'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_aggregation_builder.h': '4a4b70640d1f203eed139e0e83b53bfd2ae89df655f9fc50803dd22587d4e9b9', 'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_join_builder.cpp': 'f11a39d6425b3dcb7888a427f46cd443f35d802db9046b4f1c0e14dba21f7e6a', 'ydb/core/kqp/opt/rbo/rules/assign_stages.cpp': '0871ccc05675193dfd37120a1245d4cc7a9012dc3f5a2e2732349da19b7f8bde', 'ydb/core/kqp/provider/yql_kikimr_settings.cpp': '59c460367dc0d317cd1dd9f12e21faf5b3a48a3ec47aeaf18e960e91f7143f41', 'ydb/core/kqp/proxy_service/kqp_proxy_service.cpp': 'badb84347dd230a1d75d1c2165b020504427dd8cfc4436b8288b54699d1e8665', 'ydb/core/kqp/query_compiler/kqp_mkql_compiler.cpp': '838d2a5e7b511b8264593e7d5b75197403a5162660da066302bd98e00543264f', 'ydb/core/kqp/query_compiler/kqp_query_compiler.cpp': '4f115788047e06f8737e5bb6d4883ba4501f9b441195405a1e969d94ed66cdc4', 'ydb/core/protos/table_service_config.proto': 'ad647255a24df1060dd1fa039ef600ee0a022a1dca4ba120bba969eff456dc4e', 'ydb/core/testlib/test_client.cpp': '38213a9ae7c964f512fd4880dd079dbcb216a0b1ff76f337b31afc686a42726d', 'ydb/library/yql/dq/actors/compute/dq_sync_compute_actor_base.h': 'd9b79c29f83a286f82454578f8ff7c4ef2cae0149787ac3fb7ab4a9d330768a3', 'ydb/library/yql/dq/actors/compute/dq_task_runner_exec_ctx.cpp': 'a7935c8e902745db0e2296f518c690494988049fcf25cddce5e449deea9900c0', 'ydb/library/yql/dq/actors/spilling/channel_storage.cpp': '0aabd1c6d1931f745d488f8e8da83e9af3aeffea76f483c6233bc52d82c51056', 'ydb/library/yql/dq/actors/spilling/channel_storage_actor.cpp': 'd8e693511a5ba7d5c11aea24a906223558bf125b49282a70ad4ab7416dca0f06', 'ydb/library/yql/dq/actors/spilling/compute_storage.cpp': 'c6974de42bcf10a6b09da7771b39adad8502af3209a112633e4ff438418a074d', 'ydb/library/yql/dq/actors/spilling/compute_storage_actor.cpp': '799813ca9201ba4dc40f09d7ff0b1f84208d6ebc28facdcb8ac33a783cccf369', 'ydb/library/yql/dq/actors/spilling/spiller_factory.h': 'e180cbaa27fba1128f2ba19b1cc40e96e53796231b55e67f448854f0379893da', 'ydb/library/yql/dq/actors/spilling/spilling_file.cpp': 'c5c93a2ae25128ea054ef52f243665339cb450a509bf95c3ee9410b8f8ac753d', 'ydb/library/yql/dq/actors/spilling/spilling_file.h': '6718a3d880047cc67c6bbb91ff56b33b996c4e16649996af300b1a66e82db5d7', 'ydb/library/yql/dq/common/dq_common.h': 'dd3ac7b3326e953fcf18e9624ee8091e7fa56a7bad2b2bb639377035e4fbd7e8', 'ydb/library/yql/dq/common/dq_serialized_batch.cpp': '28997bbd711770079ca704efe846217988603c04b23d09916c45506a126b6edb', 'ydb/library/yql/dq/comp_nodes/dq_block_hash_join.cpp': 'c317a9e05d218d1597c62c20e8ba091ac2d1ab20c8f278347c96fab3255bcfd8', 'ydb/library/yql/dq/comp_nodes/dq_hash_combine.cpp': 'f42b895e345cbdcb8e49f81a61d03a3a45fe9f42c6e060cf84c8010a037eb06f', 'ydb/library/yql/dq/comp_nodes/dq_hash_operator_serdes.cpp': 'a35426438a3e1f39aaeee93e56e2408c4694998645326eb5058bab6a5b09d82b', 'ydb/library/yql/dq/comp_nodes/dq_join_common.h': '895201de7d2d09b5f90bca8aeab68c9809169b61df2acdd27b513969e4d9c701', 'ydb/library/yql/dq/comp_nodes/dq_program_builder.cpp': 'c689dd9e097fed3f5e54c786f62367b427f7a9e671c98406a560c87a3e504c55', 'ydb/library/yql/dq/comp_nodes/dq_scalar_hash_join.cpp': 'dd91d6e85af60a142fdbe1c14459a5708f059b7e3d38c79b651c72446f292cbf', 'ydb/library/yql/dq/comp_nodes/hash_join_utils/layout_converter_common.h': '2f03295a6956f73619fd7cb35f0fedae8dad1c279838f4c5ae06b1b922143f85', 'ydb/library/yql/dq/comp_nodes/hash_join_utils/mkql_bitmap.h': '7c3154135393b5c2c9e2c6b8e75f7f76e67a29413ee397813b0fa20000f99b29', 'ydb/library/yql/dq/comp_nodes/hash_join_utils/spilled_storage.cpp': '8bd34f5a7db0a0764dcd92dbf700d4ff33c3b3e14af31547a82a79d01ac8a0b3', 'ydb/library/yql/dq/comp_nodes/hash_join_utils/spilled_storage.h': '36a6dd830d82dced74464a9fcec938fda7646524c05a6a3c853a6578b5f56122', 'ydb/library/yql/dq/runtime/dq_channel_service.cpp': 'e34cf9a2949bcc92451be28f2598d0a8e0a4ccc5f8f360a60d43d9f79372e69d', 'ydb/library/yql/dq/runtime/dq_channel_service.h': '04f1ef44c16c82b251c6163ea44f47fc4bad1515ef0ec3babb526ebd61e7866e', 'ydb/library/yql/dq/runtime/dq_output_channel.cpp': '0d14558cec46c368e3f30477e5e57d51a413b77c3d42fb6fee4ad6fd2927ceea', 'ydb/library/yql/dq/runtime/dq_tasks_runner.cpp': '73785822d7d8ec6c6cd91f88a48cc52bb20de4e33c768c48901c3814ca69ca2f', 'ydb/library/yql/dq/tasks/dq_connection_builder.h': 'c8f552854d390ac96b25afd81e2cc1113e628fd0daf783abb165764872bdc02e', 'ydb/library/yql/dq/tasks/dq_task_program.cpp': '54b602ac00873ca0dd6372c45ab4853ec06d94aa5f3db9bccc436b100417b57f', 'ydb/tests/tools/kqprun/kqprun.cpp': 'c706050a14132739899130cd5a97fdb0ed0445790d8dc7cd9859fb2b2d4a8cce', 'ydb/tests/tools/kqprun/runlib/kikimr_setup.cpp': 'fb7869dadd778802c8310d49b20ccf7bc804f6f9f3959539f3b17b760105c31d', 'ydb/tests/tools/kqprun/src/ydb_setup.cpp': 'b9e908d29b8743ef617e178b727e1bffd766aaf2afc46b26ace1c155be9aefbb', 'yql/essentials/minikql/computation/mkql_computation_node_pack.cpp': '5c4ce7925f8701818b97bff40d6e8c12eda32a09a7bb1f30782b8420173f19c8', 'yql/essentials/minikql/computation/mkql_spiller_adapter.h': 'f290dd3a7d19d103731885669f13fe67a1aedef5b29ab1dbb34e6c7b1f62334c', 'yql/essentials/minikql/pack_num.h': 'eec7c0a65e3bbd7d23e3d34e3bd8f058a75892702e45595134da8d708be2a818', 'ydb/core/kqp/opt/rbo/kqp_operator.h': '2f0fff7fc230f92ef405ab7d96afa7b4875d15519ea58e9a0751aaf7707b15db', 'ydb/core/kqp/opt/rbo/kqp_rbo_compute_statistics.cpp': '679c84a75bbae282599d1c8dadba537353cc098f34d42dcbeae6711221294254', 'ydb/core/kqp/opt/rbo/kqp_rbo_rules.h': '10eeb5c4ebdff32cdb038622afbbda69ed7a1b05e59e962d38ad0aba179e819b', 'ydb/core/kqp/opt/rbo/rules/decorrelation/dependent_join_pushdown.cpp': '7c14e1724de571d9bc141b950b80074430cf067ba9a8932d2c87921fe364c466'}
CASES = ('c50_left_semi', 'c50_left_anti', 'c50_right_semi', 'c50_right_anti', 'c50_left_semi_same_type_control', 'c50_left_anti_same_type_control', 'c50_right_semi_same_type_control', 'c50_right_anti_same_type_control')
RECORDS = ({'case': 'c50_left_semi', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': 'd4f6d07d8a25b1c205412dd65bf0f69c592eed1b749d7cd9709d34700d42efa5', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_semi', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': 'd4f6d07d8a25b1c205412dd65bf0f69c592eed1b749d7cd9709d34700d42efa5', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_anti', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': '4f00e8e4d24682a425dc2f083a6eed5d9492b1741f170d6a7a704407cfd737ca', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_anti', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': '4f00e8e4d24682a425dc2f083a6eed5d9492b1741f170d6a7a704407cfd737ca', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_semi', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': '7611df5fd8b4eef168a29ab848421cf37d87c46146dd538876f057434f45c04f', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_semi', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': '7611df5fd8b4eef168a29ab848421cf37d87c46146dd538876f057434f45c04f', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_anti', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': '86d1e812a287207fb4e060f70611095952e6e151ef8aad883a39ca247c9f0ee9', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_anti', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': '86d1e812a287207fb4e060f70611095952e6e151ef8aad883a39ca247c9f0ee9', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_semi_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': 'f3929581857ed8f6dcee049fcb2b3e2f818818eda967dbc4c1442c6db720d35d', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_semi_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': 'f3929581857ed8f6dcee049fcb2b3e2f818818eda967dbc4c1442c6db720d35d', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_anti_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': 'bb978ce05a0cf27d9e6950729e762f9a026678d2fa8af855b6a7eb42e50a3aaf', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_left_anti_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': 'bb978ce05a0cf27d9e6950729e762f9a026678d2fa8af855b6a7eb42e50a3aaf', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_semi_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': '9cc5c36d9238a6bb438565bc02a97ebe6c5d833a13b4e0554774ddcd25f10337', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_semi_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': '9cc5c36d9238a6bb438565bc02a97ebe6c5d833a13b4e0554774ddcd25f10337', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_anti_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 0, 'node_count': 4, 'query_sha256': 'f633f419899ba61b87ef4dcef43345ff8525c2dead47fc5e76c66a96c78b1d9f', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'}, {'case': 'c50_right_anti_same_type_control', 'mode': 'new', 'rows': 24, 'repetition': 1, 'node_count': 4, 'query_sha256': 'f633f419899ba61b87ef4dcef43345ff8525c2dead47fc5e76c66a96c78b1d9f', 'schema_sha256': '33ff8a3ff3aad9cf91030c0cf3f001de93b205f12c6cb7db5b4ecac1c21d2119', 'fixture_sha256': ['0f3b78a522f7e42a57560e572c06e0a5d85b7fb2518751102be9288558bb4053', '054405a915381103924f388457a448a24314287d21d5d48b7c5f2bcc643c5963', '6552d7b784862b986681531b6ebff5cfbce2d6464e688aa2bab2bfdf4575a3c0'], 'settings_sha256': '80131d5f36d4cb50e29bff48fa1bac50451d6c8b50eea261ff68a8ecda40228f'})
_scope = None


def _hash(raw):
    return hashlib.sha256(raw).hexdigest()


def _source_snapshot():
    result = {}
    for name, expected in SOURCE_BINDINGS.items():
        path = (_PROJECT / name).resolve()
        raw = path.read_bytes()
        if _hash(raw) != expected:
            raise ValueError('Integration verification source requires renewed review')
        result[path] = raw
    return result


def _load_join_snapshot(snapshots):
    """Only the three existing recipe modules use this local import adapter."""
    paths = {(_ROOT/'join_key_checks.py').resolve(), (_ROOT/'window_checks.py').resolve(),
             (_PROJECT/'rbo-review-20261021/generate_and_verify.py').resolve()}
    original_import = builtins.__import__
    class Loader:
        def __init__(self, path):
            self.path = Path(path).resolve()
        def create_module(self, spec):
            return None
        def exec_module(self, module):
            module.__file__ = str(self.path)
            module.__dict__['__builtins__'] = dict(vars(builtins), __import__=local_import)
            raw = snapshots[self.path]
            exec(compile(raw, str(self.path), 'exec'), module.__dict__)
            module._verification_source_sha256 = _hash(raw)
    def spec(name, path, *args, **kwargs):
        resolved = Path(path).resolve()
        if args or kwargs or resolved not in paths:
            raise ValueError('Unexpected integration recipe import')
        return importlib.util.spec_from_loader(name, Loader(resolved), origin=str(resolved))
    proxy = ModuleType('importlib')
    proxy.util = SimpleNamespace(spec_from_file_location=spec, module_from_spec=importlib.util.module_from_spec)
    def local_import(name, globals=None, locals=None, fromlist=(), level=0):
        if name == 'importlib.util' and level == 0 and not fromlist:
            return proxy
        return original_import(name, globals, locals, fromlist, level)
    module = ModuleType('c06_c50_original_join_snapshot')
    Loader(_ROOT/'join_key_checks.py').exec_module(module)
    for item, path in ((module, _ROOT/'join_key_checks.py'), (module.w, _ROOT/'window_checks.py'),
                       (module.g, _PROJECT/'rbo-review-20261021/generate_and_verify.py')):
        if item._verification_source_sha256 != _hash(snapshots[path.resolve()]):
            raise ValueError('Integration recipe snapshot mismatch')
    return module


def _load_provenance(snapshots):
    path = (_ROOT/'binary_provenance.py').resolve()
    module = ModuleType('c06_c50_binary_provenance_snapshot')
    module.__file__ = str(path)
    exec(compile(snapshots[path], str(path), 'exec'), module.__dict__)
    return module


def initialize_integration(worktree, build_receipt, driver):
    """Dedicated-runner init API. No caller-supplied success Boolean is accepted."""
    global _scope
    if _scope is not None:
        raise ValueError('Integration scope is already initialized')
    snapshots = _source_snapshot()
    source_hash = globals().get('_verification_source_sha256')
    if source_hash != _hash(Path(__file__).read_bytes()):
        raise ValueError('Integration module must use a loaded source snapshot')
    worktree = Path(worktree).resolve(strict=True)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=worktree, text=True).strip()
    tree = subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], cwd=worktree, text=True).strip()
    if tree != EXPECTED_TREE:
        raise ValueError('Integration production tree differs from reviewed composition')
    for path, expected in PRODUCTION_BINDINGS.items():
        raw = subprocess.check_output(['git', 'show', revision + ':' + path], cwd=worktree)
        if _hash(raw) != expected:
            raise ValueError('Integration spill source differs from reviewed composition')
    provenance_module = _load_provenance(snapshots)
    provenance = provenance_module.verify_build_receipt(worktree, revision, build_receipt)
    if (not isinstance(driver, ModuleType)
        or Path(driver.__file__).resolve() != (_ROOT/'native_verify.py').resolve()
        or getattr(driver, '_verification_source_sha256', None) != SOURCE_BINDINGS['rbo-fixes-20261023/native_verify.py']
        or str(Path(driver.g.BINARY).resolve()) != provenance['binary_provenance']['resolved_path']):
        raise ValueError('Dedicated integration runner must use the audited driver and verified executable')
    recipe = _load_join_snapshot(snapshots)
    _scope = {'worktree':worktree, 'revision':revision, 'tree':tree,
              'provenance_module':provenance_module, 'provenance':copy.deepcopy(provenance),
              'recipe':recipe, 'module_sha256':source_hash, 'driver':driver}
    try:
        _require_scope()
    except BaseException:
        _scope = None
        raise
    return integration_identity()


def _require_scope():
    if _scope is None:
        raise ValueError('Dedicated verified integration runner initialization required')
    _source_snapshot()
    if (str(Path(_scope['driver'].g.BINARY).resolve()) != _scope['provenance']['binary_provenance']['resolved_path']
        or getattr(_scope['driver'], '_verification_source_sha256', None) != SOURCE_BINDINGS['rbo-fixes-20261023/native_verify.py']):
        raise ValueError('Integration driver or executable binding changed')
    if (_hash(Path(__file__).read_bytes()) != _scope['module_sha256']
        or _hash(Path(_scope['provenance']['build_receipt_path']).read_bytes()) != _scope['provenance']['build_receipt_sha256']
        or not _scope['provenance_module'].unchanged_since_verification(
            _scope['worktree'], _scope['revision'], _scope['provenance'])):
        raise ValueError('Integration executable, sources or receipt changed')
    return _scope


def integration_identity():
    scope = _require_scope()
    p = scope['provenance']
    return {'version':VERSION, 'revision':scope['revision'], 'tree':EXPECTED_TREE,
            'build_receipt_sha256':p['build_receipt_sha256'],
            'runner_sha256':p['binary_provenance']['sha256'],
            'runner_path':p['binary_provenance']['resolved_path'],
            'generator_sha256':scope['module_sha256'],
            'source_snapshot_sha256':dict(SOURCE_BINDINGS),
            'bounded_configuration':True, 'integration_only':True,
            'unchanged_configuration_claim':False,
            'final_distribution_proof':False}


def _cell(case, n, mode='new'):
    scope = _require_scope()
    if type(n) is not int or n != 24 or case not in CASES or mode != 'new':
        raise ValueError('Unreviewed integration case, size or optimizer mode')
    return scope['recipe']


def recipes(n):
    original = _cell(CASES[0], n).recipes(n)
    if any(case not in original for case in CASES):
        raise ValueError('Original integration recipe is missing')
    return {case: original[case] for case in CASES}


def metadata(case, n, mode='new'):
    original = _cell(case, n, mode)
    value = copy.deepcopy(original.requirements(case, mode))
    cfg = value['table_service_config']
    if (value['node_count'] != 4 or value['repetitions'] != 2
        or cfg.get('EnableQueryServiceSpilling') is not True
        or cfg.get('EnableSpillingInHashJoinShuffleConnections') is not True
        or cfg.get('UseBlockHashJoin') is not True or cfg.get('EnableSpillingNodes') != 'All'
        or 'SpillingServiceConfig' in cfg):
        raise ValueError('Original integration configuration changed')
    cfg.update(EnableSpillingNodes='None', SpillingServiceConfig=LOCAL_SERVICE)
    value['generator_version'] = VERSION
    if case in EMPTY_WITNESS_CASES:
        value['c06_c50_empty_result_witness'] = EMPTY_WITNESS_VERSION
    return value


def schema_and_seeds(n, case):
    return _cell(case, n).schema_and_seeds(n, case)


def check_rows(case, rows, n):
    return _cell(case, n).check_rows(case, rows, n)



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


def required_record_vector():
    _require_scope()
    return copy.deepcopy(list(RECORDS))


def validate_record(record, driver_runner):
    """Call before saving each sanitized record; outer guard is checked later."""
    scope = _require_scope()
    if (not isinstance(record, dict) or any(type(record.get(key)) is not int for key in ('repetition', 'rows', 'node_count', 'exit_code'))):
        raise ValueError('Invalid integration record field types')
    matches = [x for x in RECORDS if x['case'] == record.get('case') and x['repetition'] == record.get('repetition')]
    if len(matches) != 1:
        raise ValueError('Unexpected integration record')
    expected = matches[0]
    if any(record.get(k) != expected[k] for k in expected):
        raise ValueError('Integration workload/configuration fingerprint differs')
    if (str(Path(driver_runner).resolve()) != scope['provenance']['binary_provenance']['resolved_path']
        or record.get('generator_module') != 'c06_c50_bounded_integration_checks'
        or record.get('generator_version') != VERSION
        or record.get('generator_hash_source') != 'loaded_source_snapshot'
        or record.get('generator_sha256') != scope['module_sha256']
        or record.get('revision') != scope['revision'] or record.get('phase') != 'integration'
        or record.get('integration_scope') != integration_identity()
        or record.get('binary_provenance_verified') is not True
        or record.get('build_receipt_sha256') != scope['provenance']['build_receipt_sha256']
        or record.get('binary_provenance') != scope['provenance']['binary_provenance']
        or record.get('optimizer_mode_config_verified') is not True
        or record.get('target_results_checked') is not True
        or record.get('result_set_boundaries_known') is not True
        or record.get('expected_rejection_configured') is not False
        or record.get('table_service_overrides') != metadata(record['case'], 24)['table_service_config']):
        raise ValueError('Integration record provenance or execution scope differs')
    witness_fields = ('empty_result_set_witness_version', 'empty_result_set_metadata_verified',
                      'empty_result_set_payload_sha256', 'result_output_format')
    if record['case'] in EMPTY_WITNESS_CASES:
        if (record.get('empty_result_set_witness_version') != EMPTY_WITNESS_VERSION
            or record.get('empty_result_set_metadata_verified') is not True
            or record.get('result_output_format') != 'full-json'
            or not isinstance(record.get('empty_result_set_payload_sha256'), str)
            or len(record['empty_result_set_payload_sha256']) != 64
            or any(char not in '0123456789abcdef' for char in record['empty_result_set_payload_sha256'])
            or record.get('empty_result_set_payload_sha256') != record.get('result_sha256')
            or type(record.get('row_count')) is not int or record['row_count'] != 0
            or type(record.get('result_set_count')) is not int or record['result_set_count'] != 1):
            raise ValueError('Missing bound empty-result metadata witness')
    elif any(field in record for field in witness_fields):
        raise ValueError('Empty-result witness used outside its reviewed cases')
    # A failing semantic verdict remains a failing record, never an exception
    # class or result-shape normalization that gets relabeled a pass.
    return record.get('exit_code') == 0 and record.get('semantic_oracle_pass') is True


def validate_complete(records):
    """Dedicated runner finalizes only the exact guarded16-record vector."""
    scope = _require_scope()
    if (not isinstance(records, list) or len(records) != len(RECORDS)
        or [(x.get('case'), x.get('repetition')) for x in records]
           != [(x['case'], x['repetition']) for x in RECORDS]):
        raise ValueError('Incomplete or reordered integration vector')
    paths = {x.get('native_batch_memory_evidence') for x in records}
    if len(paths) != 1 or None in paths:
        raise ValueError('Integration batch guard binding missing')
    path = Path(next(iter(paths)))
    raw = path.read_bytes()
    memory = json.loads(raw)
    digest = _hash(raw)
    if (memory.get('exit_code') != 0 or type(memory.get('exit_code')) is not int
        or memory.get('max_scope_gib') != 24 or memory.get('min_host_available_gib') != 36
        or memory.get('cleanup_complete') is not True or memory.get('watchdog_stopped') is not False
        or memory.get('stdout_overflow') is not False
        or memory.get('scope_membership_accounting_available') is not True
        or any(x.get('native_batch_guard_pass') is not True or x.get('native_batch_memory_sha256') != digest for x in records)):
        raise ValueError('Integration batch memory guard failed')
    results = [validate_record(x, scope['driver'].g.BINARY) for x in records]
    return {'bounded_integration_semantics_pass':all(results),
            'record_count':len(records), 'semantic_pass_count':sum(results),
            'exact_original_workload_vector':True,
            'configuration_changed':True, 'original_failures_reclassified':False,
            'standalone_fix_admission_changed':False,
            'final_distribution_proof':False, 'integration_scope':integration_identity(),
            'guard_sha256':digest}
