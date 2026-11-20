#include "runner.h"
#include "logical_plan.h"
#include "signature.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/kqp/common/kqp_user_request_context.h>
#include <ydb/core/kqp/common/simple/query_ref.h>
#include <ydb/core/kqp/host/kqp_native_inspection.h>
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider.h>
#include <ydb/core/kqp/provider/yql_kikimr_results.h>
#include <ydb/core/kqp/tracing/kqp_compile_trace.h>
#include <ydb/core/scheme/scheme_type_info.h>
#include <ydb/core/scheme/scheme_type_registry.h>
#include <ydb/core/testlib/actors/test_runtime.h>
#include <ydb/tests/tools/parsed_query_bench/proto/execute_query.pb.h>
#include <yql/essentials/core/services/mounts/yql_mounts.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/minikql/mkql_function_registry.h>
#include <yql/essentials/utils/log/log.h>

#include <library/cpp/json/json_reader.h>
#include <util/generic/hash_set.h>
#include <util/generic/scope.h>
#include <util/system/file.h>

#include <set>
#include <thread>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using namespace NYql;

class TCatalog : public IKqpGateway::IKqpTableMetadataLoader {
public:
    TString Database = "/Root";

    void Load(const TString& path, const NScheme::TTypeRegistry& registry, TString& cluster, TString& prefix) {
        TFile file(path, OpenExisting | RdOnly);
        const i64 size = file.GetLength();
        Y_ENSURE(size > 0 && size <= static_cast<i64>(MaxRequestBytes), "Invalid catalog size");
        TString bytes(size, '\0');
        file.Load(bytes.Detach(), bytes.size());
        NJson::TJsonValue catalog;
        Y_ENSURE(NJson::ReadJsonTree(bytes, &catalog) && catalog["format_version"].GetUIntegerSafe() == 1,
            "Invalid offline catalog");
        cluster = catalog["cluster"].GetStringSafe();
        Database = catalog["database"].GetStringSafe();
        prefix = catalog["table_path_prefix"].GetStringSafe();
        Y_ENSURE(cluster && Database.StartsWith('/') && (prefix == Database || prefix.StartsWith(Database + "/")),
            "Invalid catalog cluster, database or table path prefix");

        std::set<std::pair<ui64, ui64>> ids;
        for (const auto& table : catalog["tables"].GetArraySafe()) {
            const auto& name = table["path"].GetStringSafe();
            const auto owner = table["owner_id"].GetUIntegerSafe();
            const auto id = table["table_id"].GetUIntegerSafe();
            const auto version = table["schema_version"].GetUIntegerSafe();
            Y_ENSURE(name.StartsWith(Database + "/") && owner && id && version && ids.emplace(owner, id).second,
                "Invalid catalog table identity: " << name);
            auto metadata = MakeIntrusive<TKikimrTableMetadata>(cluster, name);
            metadata->PathId = TKikimrPathId(owner, id);
            metadata->SchemaVersion = version;
            metadata->DoesExist = true;
            const auto& storage = table["storage"].GetStringSafe();
            Y_ENSURE(storage == "row" || storage == "column", "Unsupported catalog storage: " << storage);
            metadata->Kind = storage == "row" ? EKikimrTableKind::Datashard : EKikimrTableKind::Olap;
            metadata->StoreType = storage == "row" ? EStoreType::Row : EStoreType::Column;
            THashSet<ui32> columnIds;
            for (const auto& col : table["columns"].GetArraySafe()) {
                const auto& colName = col["name"].GetStringSafe();
                const auto colId = col["id"].GetUIntegerSafe();
                const auto& type = col["type"].GetStringSafe();
                Y_ENSURE(colName && colId && colId <= Max<ui32>() && columnIds.insert(colId).second,
                    "Invalid catalog column: " << colName);
                NScheme::TTypeInfo info;
                TString error;
                Y_ENSURE(NScheme::GetTypeInfo(registry.GetType(type), {}, type, colName, info, error), error);
                Y_ENSURE(metadata->Columns.emplace(colName,
                    TKikimrColumnMetadata(colName, colId, type, col["not_null"].GetBooleanSafe(), info)).second,
                    "Duplicate catalog column: " << colName);
                metadata->ColumnOrder.push_back(colName);
            }
            THashSet<TString> keys;
            for (const auto& key : table["key_columns"].GetArraySafe()) {
                const auto& keyName = key.GetStringSafe();
                Y_ENSURE(metadata->Columns.contains(keyName) && keys.insert(keyName).second, "Invalid catalog key: " << keyName);
                metadata->KeyColumnNames.push_back(keyName);
            }
            Y_ENSURE(!metadata->Columns.empty() && Tables.emplace(name, std::move(metadata)).second,
                "Empty or duplicate catalog table: " << name);
        }
    }

    NThreading::TFuture<IKqpGateway::TTableMetadataResult> LoadTableMetadata(const TString& cluster, const TString& table,
        const IKqpGateway::TLoadTableMetadataSettings&, const TString& database, const TIntrusiveConstPtr<NACLib::TUserToken>&) override
    {
        const auto* entry = Tables.FindPtr(table);
        Y_ENSURE(entry && (*entry)->Cluster == cluster && database == Database, "Table outside benchmark catalog: " << table);
        IKqpGateway::TTableMetadataResult result;
        result.Metadata = *entry;
        result.SetSuccess();
        return NThreading::MakeFuture(std::move(result));
    }

    TVector<NKikimrKqp::TKqpTableMetadataProto> GetCollectedSchemeData() override {
        return {};
    }

private:
    THashMap<TString, TKikimrTableMetadataPtr> Tables;
};

NFrontend::TType ScalarParameterType(const Ydb::Type& supplied) {
    const auto* item = &supplied;
    unsigned depth = 0;
    while (item->has_optional_type()) {
        Y_ENSURE(++depth <= 64, "Optional parameter type nesting exceeds 64");
        item = &item->optional_type().item();
    }
    Y_ENSURE(item->has_type_id() || item->has_decimal_type(), "Native parameter fields require scalar types");
    auto type = item->has_decimal_type()
        ? NFrontend::ParseType("Decimal(" + std::to_string(item->decimal_type().precision()) + ","
              + std::to_string(item->decimal_type().scale()) + ")")
        : NFrontend::ParseType(Ydb::Type_PrimitiveTypeId_Name(item->type_id()));
    type.OptionalDepth = depth;
    return type;
}

NFrontend::TParameterType ParameterType(const Ydb::Type& type) {
    if (!type.has_list_type()) {
        return ScalarParameterType(type);
    }
    const auto& item = type.list_type().item();
    if (!item.has_struct_type()) {
        return NFrontend::TParameterType::TList{ScalarParameterType(item)};
    }
    NFrontend::TParameterType::TRows fields;
    for (const auto& field : item.struct_type().members()) {
        fields.push_back({field.name(), ScalarParameterType(field.type())});
    }
    return NFrontend::TParameterType(std::move(fields));
}

void RunUntilCaptured(const IKqpHost::IAsyncQueryResultPtr& compilation, const bool& captured) {
    while (!compilation->HasResult() && !captured) {
        const auto ready = compilation->Continue();
        Y_ENSURE(ready.IsReady(), "Compilation requested an unavailable offline service");
        if (ready.GetValue()) {
            break;
        }
    }
    if (!captured) {
        const auto failed = compilation->GetResult();
        ythrow yexception() << "Compilation did not reach native RBO roots: " << failed.Issues().ToString();
    }
}

} // namespace

struct TRunner::TImpl {
    NScheme::TKikimrTypeRegistry TypeRegistry;
    TIntrusivePtr<NMiniKQL::IFunctionRegistry> FunctionRegistry =
        NMiniKQL::CreateFunctionRegistry(NMiniKQL::CreateBuiltinRegistry());
    NActors::TTestActorRuntime Runtime;
    TModuleResolverState Modules;
    std::shared_ptr<TCatalog> Catalog = std::make_shared<TCatalog>();
    TKqpSettings Defaults;
    TIntrusivePtr<IKqpHost> Host;
    TIntrusivePtr<IKqpGateway> Gateway;
    std::thread::id Caller;
    TString Cluster = "local";
    TString PathPrefix = "/Root";

    TKikimrConfiguration::TPtr MakeConfig() const {
        auto config = MakeIntrusive<TKikimrConfiguration>();
        config->Init(Defaults.DefaultSettings.GetDefaultSettings(), Cluster, Defaults.Settings, false);
        config->_KqpTablePathPrefix = PathPrefix;
        config->SetNativeFrontendMode(NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED);
        config->SetEnableNewRBO(true);
        config->DisableBlockExecution = true;
        config->FreezeDefaults();
        return config;
    }

    TNativeQuery ImportLogicalPlan(const Ydb::Query::Benchmark::ExecuteQueryRequest& request,
        TExprContext& ctx, TKikimrSessionContext& session) const
    {
        NFrontend::TBindSettings binding;
        binding.DefaultCluster = std::string(Cluster);
        binding.TablePathPrefix = std::string(PathPrefix);
        for (const auto& [name, value] : request.parameters()) {
            Y_ENSURE(value.has_type(), "Missing parameter type: " << name);
            std::string key(name);
            if (key.starts_with('$')) {
                key.erase(0, 1);
            }
            Y_ENSURE(binding.ParameterTypes.emplace(std::move(key), ParameterType(value.type())).second,
                "Duplicate parameter name: " << name);
        }

        TVector<TKikimrTableMetadataPtr> metadata;
        auto native = ImportNativeLogicalPlan(request.parsed_query().logical_plan(),
            [&](const NFrontend::TTableAddress& address, NFrontend::TSourcePosition) {
                Y_ENSURE(TStringBuf(address.Cluster) == Cluster, "Native query requires the configured cluster");
                IKqpGateway::TLoadTableMetadataSettings settings;
                settings.WithTableStats(true).WithAuthInfo(true).WithPrivateTables(false).WithExternalDatasources(true);
                const auto loaded = Gateway->LoadTableMetadata(Cluster, TString(address.Name), settings);
                Y_ENSURE(loaded.IsReady(), "LogicalPlan requires an offline table catalog");
                const auto& result = loaded.GetValue();
                result.ReportIssues(ctx.IssueManager);
                Y_ENSURE(result.Success(), ctx.IssueManager.GetIssues().ToString());
                const auto& meta = result.Metadata;
                Y_ENSURE(meta && meta->DoesExist, "Table does not exist: " << address.Name);
                Y_ENSURE((meta->Kind == EKikimrTableKind::Datashard || meta->Kind == EKikimrTableKind::Olap
                    || meta->Kind == EKikimrTableKind::SysView) && !meta->Temporary, "Unsupported LogicalPlan table kind");
                metadata.push_back(meta);
                auto table = MakeRboTable(*meta);
                const auto storage = meta->Kind == EKikimrTableKind::Olap
                    ? NYql::EStorageType::ColumnStorage : NYql::EStorageType::RowStorage;
                return TNativeTable{*table.NativeCatalog, std::move(table), storage};
            }, ctx, binding);

        auto& tables = session.Tables();
        const auto database = session.GetDatabase();
        for (const auto& meta : metadata) {
            tables.GetOrAddTable(Cluster, database, meta->Name).Metadata = meta;
            for (auto impl : meta->ImplTables) {
                while (impl) {
                    auto& index = tables.GetOrAddTable(Cluster, database, impl->Name);
                    tables.AddIndexImplTableToMainTableMapping(meta->Name, impl->Name);
                    index.Metadata = impl;
                    impl = impl->Next;
                }
            }
        }
        return native;
    }
};

TRunner::TRunner(const TString& catalogPath)
    : Impl(std::make_unique<TImpl>())
{
    NYql::NLog::TComponentHelpers::ForEach([](auto component) {
        NYql::NLog::YqlLogger().SetComponentLevel(component, NYql::NLog::ELevel::WARN);
    });
    auto* appData = new TAppData(0, 0, 0, 0, {}, &Impl->TypeRegistry, Impl->FunctionRegistry.Get(), nullptr, nullptr);
    Impl->Runtime.Initialize({appData, {}, {}, {}, {}});
    if (catalogPath) {
        Impl->Catalog->Load(catalogPath, Impl->TypeRegistry, Impl->Cluster, Impl->PathPrefix);
    }
    Y_ENSURE(GetYqlDefaultModuleResolver(Impl->Modules.ExprCtx, Impl->Modules.ModuleResolver),
        Impl->Modules.ExprCtx.IssueManager.GetIssues().ToString());
    auto counters = MakeIntrusive<TKqpRequestCounters>();
    const auto dynamic = Impl->Runtime.GetDynamicCounters();
    counters->Counters = new TKqpCounters(dynamic);
    counters->TxProxyMon = new NTxProxy::TTxProxyMon(dynamic);
    Impl->Gateway = CreateKikimrIcGateway(Impl->Cluster, NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY,
        Impl->Catalog->Database, Impl->Catalog->Database, Impl->Catalog, Impl->Runtime.GetActorSystem(0),
        Impl->Runtime.GetNodeId(), counters);
}

TRunner::~TRunner() = default;

void TRunner::WithSession(const std::function<void()>& run) {
    Impl->Caller = std::this_thread::get_id();
    Impl->Runtime.RunCall([&] {
        Y_ENSURE(Impl->Caller == std::this_thread::get_id() && !Impl->Host, "Invalid benchmark actor session");
        Impl->Host = CreateKqpHost(Impl->Gateway, Impl->Cluster, Impl->Catalog->Database, Impl->MakeConfig(),
            Impl->Modules.ModuleResolver, std::nullopt, nullptr, std::make_shared<TGUCSettings>(), {}, Nothing(),
            Impl->Runtime.GetAppData().FunctionRegistry);
        Y_DEFER { Impl->Host.Reset(); };
        run();
        return true;
    });
}

TCapture TRunner::Run(const Ydb::Query::Benchmark::ExecuteQueryRequest& request, bool verify) {
    Y_ENSURE(Impl->Host, "Benchmark requires an active actor session");
    TCapture result{};
    bool captured = false;
    auto trace = std::make_shared<TCompileTrace>();
    Y_DEFER { trace->CaptureRboRoots = {}; };
    trace->CaptureRboRoots = [&](const TVector<TIntrusivePtr<TOpRoot>>& roots, TExprContext& ctx) {
        result.Done = TMeasurementTime::Now();
        Y_ENSURE(std::this_thread::get_id() == Impl->Caller, "Compiler moved between thread CPU clocks");
        Y_ENSURE(!roots.empty(), "Expected native RBO roots");
        for (const auto& root : roots) {
            Y_ENSURE(root->PlanProps.NativeExpressions && root->PlanProps.ResultIndex && !root->PlanProps.WithEffects,
                "Expected a read-only result from the native frontend");
        }
        if (verify) {
            result.Signature = BuildLogicalSignature(roots, ctx);
        }
        captured = true;
    };

    IKqpHost::IAsyncQueryResultPtr compilation;
    if (request.has_query_content()) {
        std::shared_ptr<std::map<TString, Ydb::Type>> parameterTypes;
        if (!request.parameters().empty()) {
            parameterTypes = std::make_shared<std::map<TString, Ydb::Type>>();
            for (const auto& [name, value] : request.parameters()) {
                Y_ENSURE(value.has_type(), "Missing parameter type: " << name);
                parameterTypes->emplace(name, value.type());
            }
        }
        IKqpHost::TPrepareSettings settings;
        settings.CompileTrace = trace;
        compilation = Impl->Host->PrepareGenericQuery(TKqpQueryRef(request.query_content().text(), std::move(parameterTypes)), settings);
    } else {
        Y_ENSURE(request.has_parsed_query() && request.parsed_query().has_logical_plan(),
            "Expected SQL text or a logical plan");
        compilation = PrepareNativeForInspection(*Impl->Host, trace, [&](TExprContext& ctx, TKikimrSessionContext& session) {
            return Impl->ImportLogicalPlan(request, ctx, session);
        });
    }
    RunUntilCaptured(compilation, captured);
    return result;
}

} // namespace NKikimr::NKqp::NParsedQueryBench
