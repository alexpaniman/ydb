#include "kqp_rbo_table.h"
#include <ydb/core/kqp/provider/yql_kikimr_provider_impl.h>

namespace NKikimr::NKqp {

TRboTable MakeRboTable(const NYql::TKikimrTableMetadata& metadata) {
    TRboTable table(metadata.Name, metadata.PathId.ToString(), metadata.SysView, ToString(metadata.SchemaVersion));
    {
        auto columns = std::make_shared<TRboTable::TColumns>();
        for (const auto& [name, column] : metadata.Columns) {
            auto type = NFrontend::ParseType(std::string_view(column.Type.data(), column.Type.size()));
            type.OptionalDepth = !column.NotNull;
            columns->emplace(name, type);
        }
        table.NativeColumns = std::move(columns);
    }
    return table;
}

} // namespace NKikimr::NKqp
