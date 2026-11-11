#include "kqp_rbo_table.h"
#include "kqp_native_catalog.h"
#include <ydb/core/kqp/provider/yql_kikimr_provider_impl.h>

namespace NKikimr::NKqp {

TRboTable MakeRboTableIdentity(const NYql::TKikimrTableMetadata& metadata) {
    return {metadata.Name, metadata.PathId.ToString(), metadata.SysView, ToString(metadata.SchemaVersion)};
}

TRboTable MakeRboTable(const NYql::TKikimrTableMetadata& metadata) {
    auto table = MakeRboTableIdentity(metadata);
    NFrontend::TCatalogTable schema{std::string(metadata.Name), {}};
    schema.Columns.reserve(metadata.Columns.size());
    for (const auto& [name, column] : metadata.Columns) {
        const auto type = ImportNativeStorageType(column.TypeInfo, column.NotNull);
        schema.Columns.push_back({std::string(name), type});
    }
    table.NativeCatalog.emplace(std::move(schema));
    table.NativeCatalog->Validate({});
    return table;
}

} // namespace NKikimr::NKqp
