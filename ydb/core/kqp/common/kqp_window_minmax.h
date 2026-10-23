#pragma once

#include <yql/essentials/public/udf/udf_data_type.h>

namespace NKikimr::NKqp {

// These scalar selectors have the same total order in AggrMin/AggrMax and
// NUdf::CompareValues. Floating, timezone and composite values are excluded.
inline bool IsIncrementalWindowMinMaxSlot(NYql::NUdf::EDataSlot slot) {
    using NYql::NUdf::EDataSlot;
    switch (slot) {
        case EDataSlot::Bool:
        case EDataSlot::Int8: case EDataSlot::Uint8:
        case EDataSlot::Int16: case EDataSlot::Uint16:
        case EDataSlot::Int32: case EDataSlot::Uint32:
        case EDataSlot::Int64: case EDataSlot::Uint64:
        case EDataSlot::Decimal:
        case EDataSlot::Interval: case EDataSlot::Interval64:
        case EDataSlot::Date: case EDataSlot::Datetime: case EDataSlot::Timestamp:
        case EDataSlot::Date32: case EDataSlot::Datetime64: case EDataSlot::Timestamp64:
            return true;
        default:
            return false;
    }
}

} // namespace NKikimr::NKqp
