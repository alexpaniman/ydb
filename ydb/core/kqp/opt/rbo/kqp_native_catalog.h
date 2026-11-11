#pragma once

#include "frontend/expression.h"
#include <ydb/core/scheme_types/scheme_type_info.h>

namespace NKikimr::NKqp {

// Catalog loaders already decode storage type IDs and Decimal parameters.
// Import that metadata directly; SQL type spelling belongs to the frontend.
inline NFrontend::TType ImportNativeStorageType(const NScheme::TTypeInfo& storage,
    bool notNull, NFrontend::TSourcePosition position = {}) {
    using K = NFrontend::ETypeKind;
    const auto nullable = static_cast<std::uint8_t>(!notNull);
    switch (storage.GetTypeId()) {
        case NScheme::NTypeIds::Bool: return {K::Bool, nullable};
        case NScheme::NTypeIds::Int8: return {K::Int8, nullable};
        case NScheme::NTypeIds::Int16: return {K::Int16, nullable};
        case NScheme::NTypeIds::Int32: return {K::Int32, nullable};
        case NScheme::NTypeIds::Int64: return {K::Int64, nullable};
        case NScheme::NTypeIds::Uint8: return {K::Uint8, nullable};
        case NScheme::NTypeIds::Uint16: return {K::Uint16, nullable};
        case NScheme::NTypeIds::Uint32: return {K::Uint32, nullable};
        case NScheme::NTypeIds::Uint64: return {K::Uint64, nullable};
        case NScheme::NTypeIds::Float: return {K::Float, nullable};
        case NScheme::NTypeIds::Double: return {K::Double, nullable};
        case NScheme::NTypeIds::String: return {K::String, nullable};
        case NScheme::NTypeIds::Utf8: return {K::Utf8, nullable};
        case NScheme::NTypeIds::Date: return {K::Date, nullable};
        case NScheme::NTypeIds::Datetime: return {K::Datetime, nullable};
        case NScheme::NTypeIds::Timestamp: return {K::Timestamp, nullable};
        case NScheme::NTypeIds::Interval: return {K::Interval, nullable};
        case NScheme::NTypeIds::Date32: return {K::Date32, nullable};
        case NScheme::NTypeIds::Datetime64: return {K::Datetime64, nullable};
        case NScheme::NTypeIds::Timestamp64: return {K::Timestamp64, nullable};
        case NScheme::NTypeIds::Interval64: return {K::Interval64, nullable};
        case NScheme::NTypeIds::Uuid: return {K::Uuid, nullable};
        case NScheme::NTypeIds::Json: return {K::Json, nullable};
        case NScheme::NTypeIds::Yson: return {K::Yson, nullable};
        case NScheme::NTypeIds::JsonDocument: return {K::JsonDocument, nullable};
        case NScheme::NTypeIds::Decimal: {
            const auto& decimal = storage.GetDecimalType();
            return {K::Decimal, nullable, static_cast<std::uint8_t>(decimal.GetPrecision()),
                static_cast<std::uint8_t>(decimal.GetScale())};
        }
        case 0: throw NFrontend::TFrontendError(position, "catalog column has no storage type");
        default: throw NFrontend::TUnsupportedFeature(NFrontend::EFeature::Type, position,
            "Unsupported scalar type: " + std::string(NScheme::TypeName(storage.GetTypeId())));
    }
}

} // namespace NKikimr::NKqp
