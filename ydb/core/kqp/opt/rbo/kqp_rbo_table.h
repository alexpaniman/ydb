#pragma once

#include "frontend/expression.h"
#include <memory>
#include <optional>
#include <util/generic/vector.h>
#include <util/generic/hash.h>

namespace NYql { struct TKikimrTableMetadata; }

namespace NKikimr::NKqp {

// A catalog identity, not a backend expression. Native column types survive
// index selection and logical copying without allocating YQL annotations.
struct TRboTable {
    using TColumns = THashMap<TString, NFrontend::TType>;
    TString Path;
    TString PathId;
    TString SysView;
    TString Version;
    std::shared_ptr<const TColumns> NativeColumns;

    TRboTable() = default;
    TRboTable(TString path, TString pathId, TString sysView, TString version)
        : Path(std::move(path)), PathId(std::move(pathId)), SysView(std::move(sysView)), Version(std::move(version)) {}

    explicit operator bool() const { return !Path.empty(); }
};

enum class EEffectType : ui32 {
    InsertRows,
    InsertRowsIndex,
    UpdateRows,
    UpdateRowsIndex,
    UpsertRows,
    UpsertRowsIndex,
    DeleteRows,
    DeleteRowsIndex
};

struct TEffectOptions {
    std::optional<TVector<TString>> Columns;
    std::optional<TVector<TString>> ReturningColumns;
    std::optional<TVector<TString>> DefaultColumns;
    std::optional<TString> OnConflict;
    std::optional<bool> IsBatch;
    std::optional<TVector<TVector<TString>>> Settings;
};

struct TTableSinkSettings {
    TRboTable Table;
    EEffectType EffectType;
    TEffectOptions Options;
};

TRboTable MakeRboTable(const NYql::TKikimrTableMetadata& metadata);

} // namespace NKikimr::NKqp
