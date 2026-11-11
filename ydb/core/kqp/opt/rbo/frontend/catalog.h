#pragma once

#include "expression.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace NKikimr::NKqp::NFrontend {

// SQL catalog identity. Cluster and decoded path remain separate throughout
// discovery and binding; equal paths in different clusters are distinct objects.
struct TTableAddress {
    std::string Cluster;
    std::string Name;
    bool operator==(const TTableAddress&) const = default;
};

struct TTableAddressHash {
    std::size_t operator()(const TTableAddress& address) const {
        const auto cluster = std::hash<std::string>{}(address.Cluster);
        return cluster ^ (std::hash<std::string>{}(address.Name) + 0x9e3779b9 + (cluster << 6) + (cluster >> 2));
    }
};

struct TCatalogColumn {
    std::string Name;
    TType Type;
    bool operator==(const TCatalogColumn&) const = default;
};

// A missing write schema identifies a catalog object that cannot be written
// in this configuration. Storage keys and defaults belong to table metadata,
// not scalar/parameter field types.
enum class EColumnDefault : std::uint8_t { Literal, Sequence };

struct TGeneratedColumn {
    std::string Name;
    std::string Expression;
    bool Stored = true;
};

struct TWriteSchema {
    std::vector<std::string> Keys;
    std::vector<std::pair<std::string, EColumnDefault>> Defaults;
    bool HasIndexes = false;
    std::vector<TGeneratedColumn> GeneratedColumns = {};
};

struct TCatalogTable {
    std::string Name;
    std::vector<TCatalogColumn> Columns;
    std::optional<TWriteSchema> WriteSchema = {};
    // Catalog-specific explanation, consumed only when binding a write to an
    // object without WriteSchema. Reads do not depend on write capabilities.
    std::string WriteUnsupportedReason = {};
};

// Own one catalog version and its lexical name index across binding and RBO.
// Visible column order remains the catalog's order. Copies share immutable data;
// constructing a new snapshot is required to observe a changed catalog version.
class TCatalogSnapshot {
public:
    // Value-returning catalog resolvers transfer ownership through this boundary.
    TCatalogSnapshot(TCatalogTable table);

    const TCatalogTable& Get() const;
    std::span<const std::size_t> ColumnOrder() const;
    // Binding supplies the SQL source position when validating catalog names.
    void Validate(TSourcePosition position) const;
    const TCatalogColumn* FindColumn(std::string_view name) const;
    TType GetType(std::string_view name) const;

private:
    struct TData;
    std::shared_ptr<const TData> Data;
};

} // namespace NKikimr::NKqp::NFrontend
