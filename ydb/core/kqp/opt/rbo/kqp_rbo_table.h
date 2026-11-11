#pragma once

#include "frontend/catalog.h"
#include <memory>
#include <optional>
#include <util/generic/vector.h>
#include <util/generic/hash.h>

namespace NYql { struct TKikimrTableMetadata; }

namespace NKikimr::NKqp {

namespace NFrontend { struct TRowExpression; }

// A catalog identity, not a backend expression. Native column types survive
// index selection and logical copying without allocating YQL annotations.
struct TRboTable {
    TString Path;
    TString PathId;
    TString SysView;
    TString Version;
    std::optional<NFrontend::TCatalogSnapshot> NativeCatalog;

    TRboTable() = default;
    TRboTable(TString path, TString pathId, TString sysView, TString version)
        : Path(std::move(path)), PathId(std::move(pathId)), SysView(std::move(sysView)), Version(std::move(version)) {}

    explicit operator bool() const { return !Path.empty(); }

    NFrontend::TType GetColumnType(TStringBuf name) const {
        return NativeCatalog.value().GetType({name.data(), name.size()});
    }

    bool HasColumn(TStringBuf name) const {
        return NativeCatalog.value().FindColumn({name.data(), name.size()});
    }
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
    // Immutable native expressions evaluated over storage names after defaults.
    TVector<std::pair<TString, std::shared_ptr<const NFrontend::TRowExpression>>> GeneratedColumns = {};
};

struct TTableSinkSettings {
    TRboTable Table;
    EEffectType EffectType;
    TEffectOptions Options;
};

// Frontend conversion attaches its checked native catalog snapshot directly.
// Index selection uses the complete metadata adapter below.
TRboTable MakeRboTableIdentity(const NYql::TKikimrTableMetadata& metadata);
TRboTable MakeRboTable(const NYql::TKikimrTableMetadata& metadata);

} // namespace NKikimr::NKqp
