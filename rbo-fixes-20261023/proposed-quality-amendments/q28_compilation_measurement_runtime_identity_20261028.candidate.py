"""Dormant Q28 SDK transport pilot, separate from all direct-rule contracts.

The executable and all workload inputs remain in memfds. Seven of eight
existing semantic shapes form a transport pilot; the spilling-dependent shape
is pending. This is not a query-complexity benchmark or an admission gate. Recorded timings are the server's compilation counters, not RBO timers.
"""
import hashlib
import json
import os
from pathlib import Path
import re

VERSION = 'q28_sdk_compilation_pilot_runtime_bound_v2'
PROFILE = 'quality_scheduler_semantic_checks'
ROWS = 24
CASES = tuple('q28_' + name for name in (
    'right_nullable_bag', 'nested_right_bag', 'right_map_dependencies',
    'map_filter_sort_limit_control', 'conditional_guard_control',
    'full_outer_nullable_control', 'shared_spilling_control', 'shared_copied_control'))
PENDING_CASE = 'q28_shared_spilling_control'
EXECUTED_CASES = tuple(case for case in CASES if case != PENDING_CASE)
PENDING_REASON = 'local_file_service_changes_shared_channel_eligibility_RAM_only_service_unverified'
RUNTIME_REJECT_KEYS = frozenset((
    b'KIKIMR_SERVER', b'KIKIMR_TEST_DOMAIN', b'MALLOC_TRACE', b'MALLOC_CONF',
    b'HEAPPROFILE', b'HEAPCHECK', b'CPUPROFILE', b'GMON_OUT_PREFIX',
    b'ASAN_OPTIONS', b'LSAN_OPTIONS', b'MSAN_OPTIONS', b'TSAN_OPTIONS', b'UBSAN_OPTIONS'))
RUNTIME_REJECT_PREFIXES = (b'LD_', b'DYLD_', b'LLVM_PROFILE_', b'GCOV_', b'CPUPROFILE_')


def render_cpp():
    # This is a workload-independent adapter; generated SQL is never embedded.
    return r'''
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <google/protobuf/text_format.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <set>
#include <vector>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
using namespace NKikimr;
using namespace NKikimr::NKqp;
static std::string Read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("input");
    std::string value;
    char chunk[4096];
    while (input.read(chunk, sizeof(chunk)) || input.gcount()) {
        value.append(chunk, input.gcount());
        if (value.size() > 2 * 1024 * 1024) throw std::runtime_error("input_size");
    }
    if (!input.eof()) throw std::runtime_error("input_read");
    return value;
}
static void Check(bool value) { if (!value) throw std::runtime_error("check"); }
extern char** environ;
static std::string RuntimeEnvironment() {
    std::vector<std::string> entries;
    std::set<std::string> keys;
    size_t total = 0;
    for (char** current = environ; *current; ++current) {
        std::string entry(*current);
        const auto equal = entry.find('=');
        Check(equal != std::string::npos && equal > 0);
        Check(keys.insert(entry.substr(0, equal)).second);
        total += entry.size() + 1;
        Check(total <= 2 * 1024 * 1024);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end());
    std::string result;
    for (const auto& entry : entries) {
        result.append(entry);
        result.push_back('\0');
    }
    return result;
}
static std::string RuntimeAffinity() {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    Check(sched_getaffinity(0, sizeof(cpus), &cpus) == 0);
    std::string result;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &cpus)) result += std::to_string(cpu) + "\n";
    }
    Check(!result.empty());
    return result;
}
int main(int argc, char** argv) {
    if (argc < 8 || argc > 10) return 2;
    const int report = open(argv[1], O_WRONLY | O_TRUNC);
    const int quiet = open("/dev/null", O_WRONLY);
    if (report < 0 || quiet < 0) return 2;
    if (dup2(quiet, STDOUT_FILENO) < 0 || dup2(quiet, STDERR_FILENO) < 0) return 2;
    close(quiet);
    bool completed = false, statsPresent = false, compilationPresent = false, cached = false;
    bool environmentVerified = false, affinityVerified = false;
    ui64 duration = 0, cpu = 0, resultSets = 0;
    const char* phase = "runtime_identity";
    try {
        const auto expectedEnvironment = Read(argv[2]);
        const auto expectedAffinity = Read(argv[3]);
        environmentVerified = RuntimeEnvironment() == expectedEnvironment;
        affinityVerified = RuntimeAffinity() == expectedAffinity;
        Check(environmentVerified && affinityVerified);
        phase = "configuration";
        const auto configText = Read(argv[4]);
        const auto schema = Read(argv[5]);
        const auto query = Read(argv[6]);
        NKikimrConfig::TAppConfig config;
        Check(google::protobuf::TextFormat::ParseFromString(configText, &config));
        Check(config.GetTableServiceConfig().GetEnableNewRBO());
        Check(!config.GetTableServiceConfig().GetEnableFallbackToYqlOptimizer());
        Check(config.GetTableServiceConfig().HasSpillingServiceConfig());
        Check(config.GetTableServiceConfig().GetSpillingServiceConfig().HasLocalFileConfig());
        const auto& localFiles = config.GetTableServiceConfig().GetSpillingServiceConfig().GetLocalFileConfig();
        Check(localFiles.HasEnable() && !localFiles.GetEnable());
        TKikimrSettings settings(config);
        settings.SetWithSampleTables(false).SetNodeCount(1);
        phase = "startup";
        TKikimrRunner runner(settings);
        auto tableClient = runner.GetTableClient();
        auto tableSessionResult = tableClient.CreateSession().GetValueSync();
        Check(tableSessionResult.IsSuccess());
        auto tableSession = tableSessionResult.GetSession();
        phase = "schema";
        Check(tableSession.ExecuteSchemeQuery(schema).GetValueSync().IsSuccess());
        auto queryClient = runner.GetQueryClient();
        auto sessionResult = queryClient.GetSession().GetValueSync();
        Check(sessionResult.IsSuccess());
        auto session = sessionResult.GetSession();
        phase = "fixture";
        for (int index = 7; index < argc; ++index) {
            const auto result = session.ExecuteQuery(Read(argv[index]), NYdb::NQuery::TTxControl::NoTx())
                .GetValueSync();
            Check(result.IsSuccess());
        }
        phase = "target";
        const auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().StatsMode(NYdb::NQuery::EStatsMode::Basic))
            .GetValueSync();
        Check(result.IsSuccess());
        resultSets = result.GetResultSets().size();
        phase = "statistics";
        const auto& stats = result.GetStats();
        statsPresent = stats.has_value();
        Check(statsPresent);
        const auto compilation = stats->GetCompilation();
        compilationPresent = compilation.has_value();
        Check(compilationPresent);
        cached = compilation->IsFromCache();
        duration = compilation->GetDurationUs();
        cpu = compilation->GetCpuTimeUs();
        phase = "cold_sample";
        Check(!cached && duration > 0 && cpu > 0 && resultSets == 1);
        phase = "runtime_identity_final";
        environmentVerified = RuntimeEnvironment() == expectedEnvironment;
        affinityVerified = RuntimeAffinity() == expectedAffinity;
        Check(environmentVerified && affinityVerified);
        completed = true;
        phase = "complete";
    } catch (...) {
        // Raw exception messages, issues, plans and workload text never escape.
    }
    std::ostringstream output;
    output << "{\"completed\":" << (completed ? "true" : "false")
        << ",\"phase\":\"" << phase << "\",\"stats_present\":" << (statsPresent ? "true" : "false")
        << ",\"compilation_present\":" << (compilationPresent ? "true" : "false")
        << ",\"from_cache\":" << (cached ? "true" : "false")
        << ",\"server_compilation_duration_us\":" << duration
        << ",\"server_compilation_cpu_us\":" << cpu
        << ",\"result_set_count\":" << resultSets
        << ",\"runtime_environment_verified\":" << (environmentVerified ? "true" : "false")
        << ",\"runtime_affinity_verified\":" << (affinityVerified ? "true" : "false") << "}\n";
    const auto value = output.str();
    const auto written = write(report, value.data(), value.size());
    close(report);
    return written == static_cast<ssize_t>(value.size()) && completed ? 0 : 1;
}
'''


def _hash(value):
    return hashlib.sha256(value).hexdigest()


def _runtime_snapshot():
    from anonymous_cpp_metadata import MetadataError
    values = dict(os.environb)
    if any(key in RUNTIME_REJECT_KEYS or key.startswith(RUNTIME_REJECT_PREFIXES) for key in values):
        raise MetadataError('q28_runtime_environment_override_rejected')
    if any(not key or b'=' in key or b'\0' in key or b'\0' in value for key, value in values.items()):
        raise MetadataError('q28_runtime_environment_invalid')
    environment = b''.join(entry + b'\0' for entry in sorted(key + b'=' + value for key, value in values.items()))
    cpus = sorted(os.sched_getaffinity(0))
    if not environment or len(environment) > 2 * 1024 * 1024 or not cpus or cpus[-1] >= 1024:
        raise MetadataError('q28_runtime_identity_bounds_rejected')
    affinity = b''.join(str(cpu).encode('ascii') + b'\n' for cpu in cpus)
    return {'environment': environment, 'affinity': affinity}


def _runtime_identity(snapshot):
    return {'environment_sha256': _hash(snapshot['environment']),
            'environment_encoding': 'sorted_environment_entry_nul_bytes_v1',
            'main_thread_affinity_sha256': _hash(snapshot['affinity']),
            'main_thread_cpu_count': len(snapshot['affinity'].splitlines()),
            'affinity_encoding': 'ascending_cpu_decimal_newline_v1',
            'all_worker_thread_affinities_attested': False}


def _require_runtime_unchanged(snapshot):
    from anonymous_cpp_metadata import MetadataError
    if _runtime_snapshot() != snapshot:
        raise MetadataError('q28_runtime_identity_changed')


def _disable_local_spilling(config):
    from anonymous_cpp_metadata import MetadataError
    if (len(re.findall(r'\bTableServiceConfig\s*\{', config)) != 1
            or re.search(r'\b(?:SpillingServiceConfig|LocalFileConfig|_KqpEnableSpilling)\b', config)):
        raise MetadataError('q28_pilot_spill_configuration_shape_changed')
    return re.sub(r'\bTableServiceConfig\s*\{',
                  'TableServiceConfig {\n SpillingServiceConfig { LocalFileConfig { Enable: false } }',
                  config, count=1)


def _snapshot_inputs():
    import anonymous_cpp_verify as direct
    from native_verify import config_with, import_generator_snapshot
    from optimizer_mode_contract import optimizer_mode_configuration
    module = import_generator_snapshot(PROFILE)
    recipes = module.recipes(ROWS)
    if tuple(recipes) != CASES:
        raise direct.MetadataError('q28_pilot_profile_vector_changed')
    base_config = (direct.HERE.parent / 'rbo-review-20261021' / 'new.conf').read_text()
    result = []
    for case in EXECUTED_CASES:
        meta = module.metadata(case, ROWS)
        if meta.get('node_count') != 1 or meta.get('flag_pragmas') or meta.get('query_syntax', 'yql') != 'yql':
            raise direct.MetadataError('q28_pilot_profile_configuration_changed')
        overrides = meta['table_service_overrides']
        if (overrides.get('EnableQueryServiceSpilling') is not False
                or overrides.get('EnableSpillingInHashJoinShuffleConnections') is not False
                or overrides.get('EnableSpillingNodes') != 'None'):
            raise direct.MetadataError('q28_pilot_nonspilling_profile_changed')
        pragmas = dict(meta['pragmas'])
        pragmas['YqlSelect'] = 'force'
        query = ''.join('PRAGMA ' + key + '=' + module.g.quote(value) + '; ' for key, value in pragmas.items()) + recipes[case] + ';'
        config = _disable_local_spilling(config_with(base_config, meta['table_service_overrides']))
        mode = optimizer_mode_configuration(config, 'new', 'yql', pragmas, False)
        if mode['optimizer_mode_config_verified'] is not True:
            raise direct.MetadataError('q28_pilot_optimizer_mode_unverified')
        schema, seeds = module.schema_and_seeds(ROWS, case)
        if not 1 <= len(seeds) <= 3:
            raise direct.MetadataError('q28_pilot_fixture_count_changed')
        payload = {'config': config.encode(), 'schema': schema.encode(), 'query': query.encode()}
        payload.update({'seed' + str(i): (value + ';').encode() for i, value in enumerate(seeds)})
        if any(not value or len(value) > 2 * 1024 * 1024 for value in payload.values()):
            raise direct.MetadataError('q28_pilot_input_size_rejected')
        shape = {'descriptor_kind': 'generator_text_counts_not_optimizer_graph',
                 'family': case, 'target_query_bytes': len(payload['query']),
                 'select_tokens': len(re.findall(r'\bSELECT\b', recipes[case], re.I)),
                 'join_tokens': len(re.findall(r'\bJOIN\b', recipes[case], re.I)),
                 'union_tokens': len(re.findall(r'\bUNION\b', recipes[case], re.I)),
                 'shared_binding': case.startswith('q28_shared_'),
                 'fixture_rows_do_not_scale_compile_graph': True}
        result.append((payload, dict(case=case, rows=ROWS, node_count=1, mode='new', query_syntax='yql',
            generator_module=PROFILE, generator_sha256=module._verification_source_sha256,
            generator_version=meta['generator_version'], requested_stats_mode='Basic', requested_exec_mode='Execute',
            table_service_overrides=meta['table_service_overrides'], pilot_local_file_spilling_enabled=False,
            pilot_config_difference_from_semantic_profile='local_file_service_explicitly_disabled', input_sha256={key: _hash(value) for key, value in payload.items()},
            query_structure=shape, **mode)))
    return result


def parse_measurement(payload, exit_code):
    from anonymous_cpp_metadata import MetadataError
    if type(exit_code) is not int or exit_code not in (0, 1):
        raise MetadataError('q28_measurement_native_exit_rejected')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise MetadataError('duplicate_q28_measurement_field')
            result[key] = value
        return result
    if len(payload) > 4096:
        raise MetadataError('q28_measurement_too_large')
    try:
        record = json.loads(payload, object_pairs_hook=unique)
    except (ValueError, UnicodeError):
        raise MetadataError('invalid_q28_measurement_document') from None
    keys = {'completed', 'phase', 'stats_present', 'compilation_present', 'from_cache',
            'server_compilation_duration_us', 'server_compilation_cpu_us', 'result_set_count',
            'runtime_environment_verified', 'runtime_affinity_verified'}
    if not isinstance(record, dict) or set(record) != keys:
        raise MetadataError('unexpected_q28_measurement_fields')
    if any(type(record[key]) is not bool for key in ('completed', 'stats_present', 'compilation_present', 'from_cache',
                                                               'runtime_environment_verified', 'runtime_affinity_verified')):
        raise MetadataError('untyped_q28_measurement_flags')
    if any(type(record[key]) is not int or not 0 <= record[key] < 2 ** 64 for key in
           ('server_compilation_duration_us', 'server_compilation_cpu_us', 'result_set_count')):
        raise MetadataError('untyped_q28_measurement_counters')
    valid = (record['completed'] and record['phase'] == 'complete' and record['stats_present']
             and record['compilation_present'] and not record['from_cache']
             and record['server_compilation_duration_us'] > 0 and record['server_compilation_cpu_us'] > 0
             and record['result_set_count'] == 1 and exit_code == 0
             and record['runtime_environment_verified'] and record['runtime_affinity_verified'])
    if record['phase'] not in ('runtime_identity', 'configuration', 'startup', 'schema', 'fixture', 'target',
                               'statistics', 'cold_sample', 'runtime_identity_final', 'complete'):
        raise MetadataError('unknown_q28_measurement_phase')
    if record['completed'] != (exit_code == 0) or record['completed'] and not valid:
        raise MetadataError('q28_measurement_exit_or_cold_contract_mismatch')
    return dict(record, measurement_valid=valid, query_semantics_verified=False,
                optimizer_stage_timings_verified=False, quality_improvement_verified=False)


def execute_measurements(args):
    """Called only by anonymous_cpp_verify inside its existing serial_slot."""
    import anonymous_cpp_verify as direct
    from anonymous_cpp_metadata import digest_file, verify_receipt, MetadataError
    from quality_pairing import profile_fingerprint
    if (args.command != 'measure-compilation' or args.bug != 'Q28'
        or type(args.sample_index) is not int or not 0 <= args.sample_index < 64):
        raise MetadataError('q28_compilation_pilot_arguments_rejected')
    revision = direct._expected_revision('Q28', args.phase, args.worktree)
    raw_receipt = args.receipt.read_bytes()
    receipt = json.loads(raw_receipt)
    verify_receipt(receipt, args.worktree, revision)
    if receipt['metadata']['profile'] != 'rbo' or receipt['build']['target'] != 'ydb/core/kqp/ut/rbo':
        raise MetadataError('q28_pilot_requires_exact_rbo_closure')
    own_hash = digest_file(__file__)['sha256']
    profile_hash = profile_fingerprint(['q28_compilation_measurement', PROFILE])
    inputs = _snapshot_inputs()
    runtime = _runtime_snapshot()
    runtime_identity = _runtime_identity(runtime)
    source = render_cpp().encode()
    fds = {'source': direct._memfd('q28-measurement-source', source), 'object': direct._memfd('q28-measurement-object'),
           'executable': direct._memfd('q28-measurement-executable', executable=True)}
    evidence = dict(schema_version=VERSION, bug='Q28', phase=args.phase, revision=revision,
        sample_index=args.sample_index, adapter_sha256=own_hash, profile_sha256=profile_hash,
        closure_receipt_path=str(args.receipt.resolve()), closure_receipt_sha256=_hash(raw_receipt),
        generated_source_sha256=_hash(source), generated_source_bytes=len(source),
        closure_provenance_verified=True, pilot=True, full_q28_admission=False,
        query_semantics_verified=False, quality_improvement_verified=False, optimizer_stage_timings_verified=False,
        generated_files_persisted=False, records=[], intended_case_vector=list(CASES),
        executable_case_vector=list(EXECUTED_CASES), pilot_coverage_complete=False,
        pending_cases=[{'case': PENDING_CASE, 'executed': False, 'reason': PENDING_REASON}],
        runtime_identity=runtime_identity, native_runtime_attestation_required=True,
        local_file_spilling_disabled=True,
        timing_scope='server_reported_compilation_not_pure_optimizer_time',
        runner_family='fresh_TKikimrRunner_per_sample_not_kqprun',
        structural_query_scaling_measured=False, current_direct_26_contracts_unchanged=True)
    del source
    stage = 'compile'
    def process_ok(record):
        return (record['exit_code'] == 0 and record['cleanup_complete'] is True
                and record['watchdog_stopped'] is False and record['stdout_overflow'] is False)
    try:
        compilation, _ = direct.run_anonymous(receipt['metadata']['compile_argv'], receipt['metadata']['compile_cwd'], fds,
            stdout_fd='object', timeout=args.compile_timeout, environment=receipt['metadata']['compile_environment'])
        evidence['compile'] = compilation
        if not process_ok(compilation) or compilation['environment_sha256'] != direct._json_digest(receipt['metadata']['compile_environment']):
            raise MetadataError('q28_measurement_compile_failed')
        evidence['generated_object'] = direct._fd_digest(fds['object'])
        if not evidence['generated_object']['size_bytes']:
            raise MetadataError('q28_measurement_object_missing')
        stage = 'link'
        link_argv, output_transport = direct._stdout_link_arguments(receipt['metadata']['link_argv'])
        link_argv, archive_responses = direct._archive_response_arguments(link_argv, fds, receipt['files'])
        evidence.update(link_output_transport=output_transport, link_archive_responses=archive_responses)
        linking, _ = direct.run_anonymous(link_argv, receipt['metadata']['link_cwd'], fds, stdout_fd='executable',
            timeout=args.link_timeout, max_scope_gib=36, environment=receipt['metadata']['link_environment'])
        evidence['link'] = linking
        if not process_ok(linking) or linking['environment_sha256'] != direct._json_digest(receipt['metadata']['link_environment']):
            raise MetadataError('q28_measurement_link_failed')
        for response in archive_responses['responses']:
            if direct._fd_digest(fds[response['descriptor']]) != response['content']:
                raise MetadataError('q28_measurement_link_response_changed')
        evidence['generated_executable'] = direct._fd_digest(fds['executable'])
        if not evidence['generated_executable']['size_bytes']:
            raise MetadataError('q28_measurement_executable_missing')
        os.fchmod(fds['executable'], 0o700)
        stage = 'native'
        for payload, descriptor in inputs:
            _require_runtime_unchanged(runtime)
            payload = dict(runtime_environment=runtime['environment'], runtime_affinity=runtime['affinity'], **payload)
            evidence['active_case'] = descriptor['case']
            sample_fds = {'executable': fds['executable'], 'report': direct._memfd('q28-measurement-report')}
            try:
                sample_fds.update({key: direct._memfd('q28-' + key, value) for key, value in payload.items()})
                command = ['@FD:executable@', '@FD:report@'] + ['@FD:' + key + '@' for key in payload]
                native, _ = direct.run_anonymous(command, args.worktree, sample_fds, timeout=args.native_timeout,
                    max_scope_gib=12, compiler_environment=False, retain_stdout=0)
                evidence['last_native'] = native
                _require_runtime_unchanged(runtime)
                if native['watchdog_stopped'] or native['stdout_overflow'] or not native['cleanup_complete']:
                    raise MetadataError('q28_measurement_native_guard_failed')
                for key, value in payload.items():
                    if direct._fd_digest(sample_fds[key]) != {'sha256': _hash(value), 'size_bytes': len(value)}:
                        raise MetadataError('q28_measurement_input_changed')
                result = parse_measurement(os.pread(sample_fds['report'], 4097, 0), native['exit_code'])
                evidence['records'].append(dict(descriptor, native=native, **result))
                if not result['measurement_valid']:
                    break
            finally:
                for key, fd in sample_fds.items():
                    if key != 'executable':
                        os.close(fd)
        stage = 'final_provenance'
        _require_runtime_unchanged(runtime)
        verify_receipt(receipt, args.worktree, revision)
        if (args.receipt.read_bytes() != raw_receipt or digest_file(__file__)['sha256'] != own_hash
            or profile_fingerprint(['q28_compilation_measurement', PROFILE]) != profile_hash
            or direct._fd_digest(fds['executable']) != evidence['generated_executable']):
            raise MetadataError('q28_measurement_final_provenance_changed')
        success = (len(evidence['records']) == len(EXECUTED_CASES)
                   and all(record['measurement_valid'] for record in evidence['records']))
        evidence['status'] = 'q28_compilation_pilot_partial_recorded' if success else 'q28_compilation_pilot_incomplete'
        return evidence, 0 if success else 2
    except MetadataError as error:
        error.verification_stage = 'q28_compilation_pilot_' + stage
        error.stage_evidence = {key: evidence[key] for key in
            ('compile', 'link', 'generated_object', 'generated_executable', 'active_case', 'last_native', 'runtime_identity') if key in evidence}
        raise
    finally:
        for fd in fds.values():
            os.close(fd)
