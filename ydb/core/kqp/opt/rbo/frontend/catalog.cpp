#include "catalog.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace NKikimr::NKqp::NFrontend {

struct TCatalogSnapshot::TData {
    explicit TData(TCatalogTable table) : Table(std::move(table)), Order(Table.Columns.size()) {
        std::iota(Order.begin(), Order.end(), std::size_t{0});
        std::sort(Order.begin(), Order.end(), [&](auto a, auto b) { return Table.Columns[a].Name < Table.Columns[b].Name; });
    }

    TCatalogTable Table;
    std::vector<std::size_t> Order;
};

TCatalogSnapshot::TCatalogSnapshot(TCatalogTable table)
    : Data(std::make_shared<const TData>(std::move(table))) {}

const TCatalogTable& TCatalogSnapshot::Get() const { return Data->Table; }

std::span<const std::size_t> TCatalogSnapshot::ColumnOrder() const { return Data->Order; }

void TCatalogSnapshot::Validate(TSourcePosition position) const {
    const auto& table = Get();
    if (table.Name.empty()) throw TFrontendError(position, "catalog returned an unnamed table");
    const auto& order = Data->Order;
    for (std::size_t i = 1; i < order.size(); ++i) {
        const auto& name = table.Columns[order[i]].Name;
        if (name == table.Columns[order[i - 1]].Name)
            throw TFrontendError(position, "duplicate catalog column: " + name);
    }
}

const TCatalogColumn* TCatalogSnapshot::FindColumn(std::string_view name) const {
    const auto& columns = Get().Columns;
    const auto& order = Data->Order;
    const auto found = std::lower_bound(order.begin(), order.end(), name,
        [&](auto index, std::string_view key) { return columns[index].Name < key; });
    return found != order.end() && columns[*found].Name == name ? &columns[*found] : nullptr;
}

TType TCatalogSnapshot::GetType(std::string_view name) const {
    if (const auto* column = FindColumn(name)) return column->Type;
    throw std::out_of_range("unknown catalog column: " + std::string(name));
}

} // namespace NKikimr::NKqp::NFrontend
