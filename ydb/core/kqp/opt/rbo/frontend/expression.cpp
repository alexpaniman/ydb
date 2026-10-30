#include "expression.h"

#include <contrib/libs/double-conversion/double-conversion/string-to-double.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace NKikimr::NKqp::NFrontend {
namespace {

using K = ETypeKind;
using O = EOpcode;

[[noreturn]] void Fail(TSourcePosition position, std::string message) {
    throw TFrontendError(position, std::move(message));
}

std::string Lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
        }
    }
    return result;
}

bool IsUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned remaining;
        std::uint32_t codepoint, minimum;
        if (first >= 0xC2 && first <= 0xDF) {
            remaining = 1; codepoint = first & 0x1F; minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            remaining = 2; codepoint = first & 0x0F; minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            remaining = 3; codepoint = first & 0x07; minimum = 0x10000;
        } else return false;
        if (remaining > text.size() - i) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    }
    return true;
}

int Width(K kind) {
    switch (kind) {
        case K::Int8: case K::Uint8: return 8;
        case K::Int16: case K::Uint16: return 16;
        case K::Int32: case K::Uint32: return 32;
        case K::Int64: case K::Uint64: return 64;
        default: return 0;
    }
}

K IntegerKind(int width, bool isSigned) {
    if (isSigned) {
        return width <= 8 ? K::Int8 : width <= 16 ? K::Int16 : width <= 32 ? K::Int32 : K::Int64;
    }
    return width <= 8 ? K::Uint8 : width <= 16 ? K::Uint16 : width <= 32 ? K::Uint32 : K::Uint64;
}

bool IsDate(K kind) {
    return kind == K::Date || kind == K::Datetime || kind == K::Timestamp;
}

constexpr std::int64_t MicrosPerDay = 86'400'000'000LL;
constexpr std::int64_t TimestampLimit = 49'673LL * MicrosPerDay;

std::int64_t DateUnit(K kind) {
    return kind == K::Date ? MicrosPerDay : kind == K::Datetime ? 1'000'000 : 1;
}

bool FitsTemporal(__int128 value, K kind) {
    return kind == K::Interval ? value > -TimestampLimit && value < TimestampLimit :
        value >= 0 && value < TimestampLimit / DateUnit(kind);
}

std::chrono::year_month_day CalendarDate(std::uint64_t micros) {
    return std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{micros / MicrosPerDay}}};
}

std::optional<std::uint64_t> ShiftCalendar(std::uint64_t micros, std::int64_t amount, bool years) {
    using namespace std::chrono;
    const auto date = CalendarDate(micros);
    std::int64_t y = int(date.year()), m = unsigned(date.month());
    // Preserve the existing narrow calendar's 12-bit year field, including
    // wraparound for large shifts, then apply its normal date-range check.
    if (years) y = (y + amount) & 4095;
    else {
        const auto next = m + amount;
        y = (y + (next - 1) / 12) & 4095;
        m = 1 + (next - 1) % 12;
        if (m <= 0) { y = (y - 1) & 4095; m += 12; }
    }
    if (y < 1970 || y > 2105) return {};
    const auto ym = year{int(y)} / month{unsigned(m)};
    const auto d = std::min(unsigned(date.day()), unsigned(year_month_day_last{ym / last}.day()));
    const auto days = sys_days{ym / day{d}}.time_since_epoch().count();
    const auto result = days * MicrosPerDay + micros % MicrosPerDay;
    return result < TimestampLimit ? std::optional<std::uint64_t>{result} : std::nullopt;
}

std::optional<std::int64_t> ParseTemporal(std::string_view text, K kind) {
    if (kind == K::Interval) {
        bool negative = false;
        if (text.starts_with('-')) {
            negative = true;
            text.remove_prefix(1);
        }
        if (!text.starts_with('P')) return {};
        text.remove_prefix(1);
        bool time = false, any = false;
        __int128 micros = 0;
        unsigned previous = 0;
        while (!text.empty()) {
            if (text.front() == 'T' && !time) {
                time = true;
                text.remove_prefix(1);
                if (text.empty()) return {};
                continue;
            }
            std::uint64_t whole = 0;
            const auto number = std::from_chars(text.data(), text.data() + text.size(), whole);
            if (number.ec != std::errc{} || number.ptr == text.data()) return {};
            text.remove_prefix(number.ptr - text.data());
            std::uint64_t fraction = 0;
            unsigned digits = 0;
            if (text.starts_with('.')) {
                text.remove_prefix(1);
                while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
                    if (++digits > 6) return {};
                    fraction = fraction * 10 + (text.front() - '0');
                    text.remove_prefix(1);
                }
                if (!digits) return {};
                while (digits++ < 6) fraction *= 10;
            }
            if (text.empty()) return {};
            const char unit = text.front();
            text.remove_prefix(1);
            const unsigned order = !time && unit == 'D' ? 1 : time && unit == 'H' ? 2 : time && unit == 'M' ? 3 : time && unit == 'S' ? 4 : 0;
            if (!order || order <= previous || (digits && unit != 'S')) return {};
            previous = order;
            const std::int64_t scale = unit == 'D' ? MicrosPerDay : unit == 'H' ? 3'600'000'000LL : unit == 'M' ? 60'000'000LL : 1'000'000LL;
            micros += static_cast<__int128>(whole) * scale + fraction;
            if (micros >= TimestampLimit) return {};
            any = true;
        }
        if (!any) return {};
        return static_cast<std::int64_t>(negative ? -micros : micros);
    }
    // Match the scalar Date/Datetime/Timestamp literal contract: bounded
    // numeric components, a mandatory timezone for time values, and truncation
    // of timestamp fractional digits beyond microseconds.
    std::size_t offset = 0;
    auto component = [&](unsigned maxDigits) -> int {
        unsigned digits = 0;
        int value = 0;
        while (digits < maxDigits && offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
            value = value * 10 + text[offset++] - '0';
            ++digits;
        }
        return digits ? value : -1;
    };
    auto consume = [&](char ch) {
        if (offset == text.size() || text[offset] != ch) return false;
        ++offset;
        return true;
    };
    const int year = component(4);
    if (!consume('-')) return {};
    const int month = component(2);
    if (!consume('-')) return {};
    const int day = component(2);
    if (year < 1970 || month < 1 || month > 12 || day < 1) return {};
    constexpr int MonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > MonthDays[month - 1] + (month == 2 && leap)) return {};
    // Gregorian calendar to days since 1970-01-01; bounded four-digit years.
    const int y = year - (month <= 2), era = y / 400, yearOfEra = y - era * 400;
    const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t days = era * 146097 + yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear - 719468;
    // The local date itself must be valid, even if applying a timezone offset
    // would otherwise move an out-of-range local date into the UTC range.
    if (!FitsTemporal(days, K::Date)) return {};
    std::int64_t micros = days * MicrosPerDay;
    if (kind == K::Date) {
        return offset == text.size() ? std::optional<std::int64_t>{days} : std::nullopt;
    }
    if (!consume('T')) return {};
    const int hour = component(2);
    if (!consume(':')) return {};
    const int minute = component(2);
    if (!consume(':')) return {};
    const int second = component(2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return {};
    micros += (hour * 3600LL + minute * 60 + second) * 1'000'000;
    if (consume('.')) {
        if (kind != K::Timestamp) return {};
        unsigned digits = 0;
        std::int64_t fraction = 0;
        while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
            if (digits < 6) {
                fraction = fraction * 10 + text[offset] - '0';
                ++digits;
            }
            ++offset;
        }
        if (!digits) return {};
        while (digits++ < 6) fraction *= 10;
        micros += fraction;
    }
    if (!consume('Z')) {
        if (offset == text.size() || (text[offset] != '+' && text[offset] != '-')) return {};
        const bool negative = text[offset++] == '-';
        const int hours = component(2);
        if (!consume(':')) return {};
        const int minutes = component(2);
        if (hours < 0 || minutes < 0) return {};
        // The established grammar bounds each offset component to two digits;
        // it does not restrict them to a civil-time hour/minute range.
        const std::int64_t timezone = (hours * 60LL + minutes) * 60 * 1'000'000;
        micros += negative ? timezone : -timezone;
    }
    if (offset != text.size() || micros < 0 || micros >= TimestampLimit) return {};
    return micros / DateUnit(kind);
}

void ValidateType(TType type, TSourcePosition position) {
    if (type.Kind == K::Decimal) {
        if (!type.Precision || type.Precision > 35 || type.Scale > type.Precision) {
            Fail(position, "Decimal requires 1 <= precision <= 35 and scale <= precision");
        }
    } else if (type.Precision || type.Scale) {
        Fail(position, "Precision and scale are only valid for Decimal");
    }
}

__int128 Power10(unsigned power) {
    __int128 result = 1;
    for (unsigned i = 0; i < power; ++i) {
        result *= 10;
    }
    return result;
}

TType ComputeCommonType(TType a, TType b, TSourcePosition position) {
    if (a.Kind == K::Null) {
        return b.Optional();
    }
    if (b.Kind == K::Null) {
        return a.Optional();
    }
    const bool nullable = a.Nullable || b.Nullable;
    a.Nullable = b.Nullable = nullable;
    if (a == b) {
        return a;
    }
    if (a.IsString() && b.IsString()) {
        return {K::String, nullable};
    }
    if (!a.IsNumeric() || !b.IsNumeric()) {
        Fail(position, "No common type for " + a.ToString() + " and " + b.ToString());
    }
    if (a.Kind == K::Decimal || b.Kind == K::Decimal) {
        if (a.Kind == K::Float || a.Kind == K::Double || b.Kind == K::Float || b.Kind == K::Double) {
            Fail(position, "Decimal and floating point require an explicit cast");
        }
        const unsigned scale = std::max(a.Scale, b.Scale);
        auto integralDigits = [](TType type) -> unsigned {
            if (type.Kind == K::Decimal) {
                return type.Precision - type.Scale;
            }
            // SQL reserves the same decimal width for signed/unsigned integers.
            return (Width(type.Kind) * 30103U) / 100000U + 1;
        };
        const unsigned precision = std::max(integralDigits(a), integralDigits(b)) + scale;
        if (precision > 35) {
            Fail(position, "Decimal common type exceeds precision 35");
        }
        return {K::Decimal, nullable, static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale)};
    }
    if (a.Kind == K::Double || b.Kind == K::Double || a.Kind == K::Float || b.Kind == K::Float) {
        return {a.Kind == K::Float && b.Kind == K::Float ? K::Float : K::Double, nullable};
    }
    int width = std::max(Width(a.Kind), Width(b.Kind));
    const bool isSigned = a.IsSigned() || b.IsSigned();
    if (a.IsSigned() != b.IsSigned()) {
        const int unsignedWidth = a.IsSigned() ? Width(b.Kind) : Width(a.Kind);
        if (unsignedWidth == 64) {
            throw TUnsupportedFeature(EFeature::Type, position, "implicit signed/unsigned promotion is outside the native profile");
        }
        width = std::max(width, unsignedWidth * 2);
    }
    return {IntegerKind(width, isSigned), nullable};
}

bool IsComparison(O op) {
    return op == O::Equal || op == O::NotEqual || op == O::Less || op == O::LessEqual || op == O::Greater || op == O::GreaterEqual;
}

TValue NullValue(TType type) {
    return {type.Optional(), std::monostate{}};
}

bool FitsSigned(__int128 value, int width) {
    return value >= -(__int128{1} << (width - 1)) && value < (__int128{1} << (width - 1));
}

bool FitsUnsigned(__int128 value, int width) {
    return value >= 0 && value < (__int128{1} << width);
}

TScalar WrapInteger(unsigned __int128 value, TType type) {
    const int width = type.Kind == K::Interval ? 64 : Width(type.Kind);
    const auto mask = (static_cast<unsigned __int128>(1) << width) - 1;
    std::uint64_t bits = static_cast<std::uint64_t>(value & mask);
    if (type.IsSigned() || type.Kind == K::Interval) {
        if (width < 64 && (bits & (std::uint64_t{1} << (width - 1)))) {
            bits |= ~static_cast<std::uint64_t>(mask);
        }
        return std::bit_cast<std::int64_t>(bits);
    }
    return bits;
}

bool FitsDecimal(__int128 value, TType type) {
    const auto limit = Power10(type.Precision);
    return value > -limit && value < limit;
}

bool IsDecimalSpecial(__int128 value) {
    return value == TDecimal::Inf() || value == -TDecimal::Inf() || value == TDecimal::NaN();
}

__int128 BoundDecimal(__int128 value, TType type) {
    if (FitsDecimal(value, type)) return value;
    return value < 0 ? -TDecimal::Inf() : TDecimal::Inf();
}

bool IsCanonicalDecimalText(std::string_view text) {
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) text.remove_prefix(1);
    if (const auto lower = Lower(text); lower == "inf" || lower == "nan") return true;
    bool digit = false, dot = false;
    for (const char c : text) {
        if (c == '.' && !dot) dot = true;
        else if (c >= '0' && c <= '9') digit = true;
        else return false;
    }
    return digit;
}

template <class T>
T DecimalFloat(__int128 value, unsigned scale) {
    if (value == TDecimal::NaN()) return std::numeric_limits<T>::quiet_NaN();
    if (value == TDecimal::Inf()) return std::numeric_limits<T>::infinity();
    if (value == -TDecimal::Inf()) return -std::numeric_limits<T>::infinity();
    while (scale && value % 10 == 0) { value /= 10; --scale; }
    return static_cast<T>(value) / static_cast<T>(Power10(scale));
}

std::string Int128ToString(__int128 value) {
    const bool negative = value < 0;
    // Decimal values have at most 35 digits, so negation cannot overflow.
    if (negative) {
        value = -value;
    }
    std::string text;
    do {
        text.push_back('0' + value % 10);
        value /= 10;
    } while (value);
    if (negative) {
        text.push_back('-');
    }
    std::reverse(text.begin(), text.end());
    return text;
}

// Decimal coefficients have at most 35 digits. A product can have 70, but
// long division needs only a 36-digit remainder and therefore fits __int128.
// Keeping this tiny reference executor in decimal digits avoids a bigint
// dependency and reproduces the runtime's nearest-even scale reduction.
std::optional<__int128> RoundedDecimalQuotient(std::string_view digits, __int128 divisor, bool negative) {
    __int128 quotient = 0, remainder = 0;
    for (char digit : digits) {
        remainder = remainder * 10 + digit - '0';
        const auto next = remainder / divisor;
        remainder %= divisor;
        if (__builtin_mul_overflow(quotient, __int128{10}, &quotient) ||
            __builtin_add_overflow(quotient, next, &quotient)) return {};
    }
    if (remainder * 2 > divisor || (remainder * 2 == divisor && quotient % 2)) {
        if (__builtin_add_overflow(quotient, __int128{1}, &quotient)) return {};
    }
    return negative ? -quotient : quotient;
}

std::optional<__int128> DecimalProduct(__int128 left, __int128 right, unsigned scale) {
    const auto a = Int128ToString(left < 0 ? -left : left);
    const auto b = Int128ToString(right < 0 ? -right : right);
    std::vector<unsigned> digits(a.size() + b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            digits[i + j] += (a[a.size() - i - 1] - '0') * (b[b.size() - j - 1] - '0');
        }
    }
    for (std::size_t i = 0; i + 1 < digits.size(); ++i) {
        digits[i + 1] += digits[i] / 10;
        digits[i] %= 10;
    }
    std::string product;
    product.reserve(digits.size());
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) product.push_back('0' + *it);
    return RoundedDecimalQuotient(product, Power10(scale), (left < 0) != (right < 0));
}

std::optional<__int128> ParseDecimal(std::string_view text, TType type) {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) return {};
    if (const auto lower = Lower(text); lower == "nan") return TDecimal::NaN();
    else if (lower == "inf") return negative ? -TDecimal::Inf() : TDecimal::Inf();
    __int128 result = 0;
    bool overflow = false;
    unsigned fractional = 0;
    bool dot = false;
    bool digit = false;
    int discarded = -1;
    bool discardedTail = false;
    for (char ch : text) {
        if (ch == '.' && !dot) {
            dot = true;
            continue;
        }
        if (ch < '0' || ch > '9') {
            return {};
        }
        digit = true;
        if (dot && fractional >= type.Scale) {
            if (discarded < 0) discarded = ch - '0';
            else discardedTail |= ch != '0';
            continue;
        }
        if (!overflow) overflow = __builtin_mul_overflow(result, __int128{10}, &result) ||
            __builtin_add_overflow(result, __int128{ch - '0'}, &result);
        fractional += dot;
    }
    if (!digit) return {};
    if (!overflow) overflow = __builtin_mul_overflow(result, Power10(type.Scale - fractional), &result);
    if (!overflow && (discarded > 5 || (discarded == 5 && (discardedTail || result % 2))))
        overflow = __builtin_add_overflow(result, __int128{1}, &result);
    if (overflow) return negative ? -TDecimal::Inf() : TDecimal::Inf();
    if (negative) {
        result = -result;
    }
    return BoundDecimal(result, type);
}

std::optional<__int128> IntegralValue(const TValue& value) {
    if (auto p = std::get_if<std::int64_t>(&value.Data)) {
        return *p;
    }
    if (auto p = std::get_if<std::uint64_t>(&value.Data)) {
        return *p;
    }
    if (auto p = std::get_if<bool>(&value.Data)) {
        return *p ? 1 : 0;
    }
    if (auto p = std::get_if<TDecimal>(&value.Data)) {
        if (IsDecimalSpecial(p->Coefficient)) return {};
        const auto rounded = RoundedDecimalQuotient(Int128ToString(p->Coefficient < 0 ? -p->Coefficient : p->Coefficient),
            Power10(value.Type.Scale), p->Coefficient < 0);
        // The runtime first rounds to Decimal(p-s,0), then converts to integer.
        const TType integral{K::Decimal, false, static_cast<std::uint8_t>(value.Type.Precision - value.Type.Scale), 0};
        return rounded && FitsDecimal(*rounded, integral) ? rounded : std::nullopt;
    }
    if (auto p = std::get_if<double>(&value.Data)) {
        if (std::isfinite(*p) && *p >= -std::ldexp(1.0, 63) && *p < std::ldexp(1.0, 64)) {
            return static_cast<__int128>(*p);
        }
        return {};
    }
    if (auto p = std::get_if<std::string>(&value.Data)) {
        auto text = std::string_view(*p);
        if (!text.empty() && (text.front() == '+' || text.front() == '-')) text.remove_prefix(1);
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return {};
        return ParseDecimal(*p, {K::Decimal, false, 35, 0});
    }
    return {};
}

std::optional<double> FloatingValue(const TValue& value) {
    if (auto p = std::get_if<double>(&value.Data)) {
        return *p;
    }
    if (auto p = std::get_if<TDecimal>(&value.Data)) {
        return DecimalFloat<double>(p->Coefficient, value.Type.Scale);
    }
    if (auto p = std::get_if<std::string>(&value.Data)) {
        auto special = Lower(*p);
        const bool negative = special.starts_with('-');
        if (special.starts_with('-') || special.starts_with('+')) special.erase(0, 1);
        if (special == "inf" || special == "nan") {
            const double result = special == "inf" ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
            return negative ? -result : result;
        }
        // Match the runtime's numeric text contract with the generic numeric
        // parser: leading spaces and hexadecimal integers are accepted, as are
        // overflow/underflow to infinity/zero. No locale or YQL parser is used.
        using TParser = double_conversion::StringToDoubleConverter;
        static const TParser parser(TParser::ALLOW_TRAILING_JUNK | TParser::ALLOW_HEX | TParser::ALLOW_LEADING_SPACES,
            0.0, std::numeric_limits<double>::quiet_NaN(), nullptr, nullptr);
        if (p->empty() || p->size() > std::numeric_limits<int>::max()) return {};
        int consumed = 0;
        const auto result = parser.StringToDouble(p->data(), static_cast<int>(p->size()), &consumed);
        return consumed == static_cast<int>(p->size()) ? std::optional<double>{result} : std::nullopt;
    }
    const auto integral = IntegralValue(value);
    return integral ? std::optional<double>{static_cast<double>(*integral)} : std::nullopt;
}

TValue ConvertValue(const TValue& value, TType target) {
    if (value.IsNull()) {
        return NullValue(target);
    }
    if (value.Type.Required() == target.Required()) {
        return {target, value.Data};
    }
    if (target.IsString()) {
        auto text = FormatValue(value);
        if (value.Type.Kind == K::Decimal && value.Type.Scale) {
            while (text.back() == '0') text.pop_back();
            if (text.back() == '.') text.pop_back();
        }
        if (target.Kind == K::Utf8 && !IsUtf8(text)) return NullValue(target);
        return {target, std::move(text)};
    }
    if (target.Kind == K::Bool) {
        if (const auto* string = std::get_if<std::string>(&value.Data)) {
            const auto lowered = Lower(*string);
            if (lowered == "true" || lowered == "false") {
                return {target, lowered == "true"};
            }
        } else if (const auto* number = std::get_if<double>(&value.Data)) {
            const int category = value.Type.Kind == K::Float ? std::fpclassify(static_cast<float>(*number)) : std::fpclassify(*number);
            if (category == FP_NAN) return NullValue(target);
            return {target, category != FP_ZERO && category != FP_SUBNORMAL};
        } else if (auto number = FloatingValue(value)) {
            return {target, *number != 0};
        }
        return NullValue(target);
    }
    if (target.Kind == K::Decimal) {
        if (auto decimal = std::get_if<TDecimal>(&value.Data)) {
            auto coefficient = decimal->Coefficient;
            if (IsDecimalSpecial(coefficient)) return {target, *decimal};
            if (target.Scale < value.Type.Scale) {
                coefficient = *RoundedDecimalQuotient(Int128ToString(coefficient < 0 ? -coefficient : coefficient),
                    Power10(value.Type.Scale - target.Scale), coefficient < 0);
            } else if (__builtin_mul_overflow(coefficient, Power10(target.Scale - value.Type.Scale), &coefficient)) {
                coefficient = decimal->Coefficient < 0 ? -TDecimal::Inf() : TDecimal::Inf();
            }
            return {target, TDecimal{BoundDecimal(coefficient, target)}};
        }
        if (auto decimal = ParseDecimal(FormatValue(value), target)) {
            return {target, TDecimal{*decimal}};
        }
        return NullValue(target);
    }
    if (target.Kind == K::Double || target.Kind == K::Float) {
        if (target.Kind == K::Float) {
            if (const auto* decimal = std::get_if<TDecimal>(&value.Data))
                return {target, static_cast<double>(DecimalFloat<float>(decimal->Coefficient, value.Type.Scale))};
            // Convert integers directly: an intermediate Double can round a
            // large integer onto a Float halfway point and change the result.
            if (const auto* integer = std::get_if<std::int64_t>(&value.Data))
                return {target, static_cast<double>(static_cast<float>(*integer))};
            if (const auto* integer = std::get_if<std::uint64_t>(&value.Data))
                return {target, static_cast<double>(static_cast<float>(*integer))};
        }
        if (auto number = FloatingValue(value)) {
            const double result = target.Kind == K::Float ? static_cast<float>(*number) : *number;
            return {target, result};
        }
        return NullValue(target);
    }
    if (target.IsIntegral() || target.Kind == K::Interval || IsDate(target.Kind)) {
        if (IsDate(value.Type.Kind) && IsDate(target.Kind)) {
            const auto micros = static_cast<__int128>(std::get<std::uint64_t>(value.Data)) * DateUnit(value.Type.Kind);
            return {target, static_cast<std::uint64_t>(micros / DateUnit(target.Kind))};
        }
        if ((target.Kind == K::Interval || IsDate(target.Kind)) && std::holds_alternative<std::string>(value.Data)) {
            if (const auto parsed = ParseTemporal(std::get<std::string>(value.Data), target.Kind)) {
                return target.Kind == K::Interval ? TValue{target, *parsed} : TValue{target, static_cast<std::uint64_t>(*parsed)};
            }
            return NullValue(target);
        }
        if (target.IsIntegral()) {
            if (!target.IsSigned()) {
                if (const auto* text = std::get_if<std::string>(&value.Data); text && text->starts_with('-')) return NullValue(target);
            }
            if (const auto* number = std::get_if<double>(&value.Data)) {
                const int category = value.Type.Kind == K::Float ? std::fpclassify(static_cast<float>(*number)) : std::fpclassify(*number);
                if (category == FP_ZERO || category == FP_SUBNORMAL) {
                    return target.IsSigned() ? TValue{target, std::int64_t{0}} : TValue{target, std::uint64_t{0}};
                }
                const int width = Width(target.Kind);
                const __int128 minimum = target.IsSigned() ? -(__int128{1} << (width - 1)) : 0;
                const __int128 maximum = (__int128{1} << (width - target.IsSigned())) - 1;
                if (!std::isfinite(*number) || static_cast<long double>(*number) < static_cast<long double>(minimum) ||
                    static_cast<long double>(*number) > static_cast<long double>(maximum)) return NullValue(target);
            }
        }
        if (auto number = IntegralValue(value)) {
            if ((target.Kind == K::Interval || IsDate(target.Kind)) && !FitsTemporal(*number, target.Kind)) {
                return NullValue(target);
            }
            const bool isSigned = target.IsSigned() || target.Kind == K::Interval;
            const int width = target.Kind == K::Date ? 16 : target.Kind == K::Datetime ? 32 :
                target.Kind == K::Timestamp || target.Kind == K::Interval ? 64 : Width(target.Kind);
            if (isSigned ? FitsSigned(*number, width) : FitsUnsigned(*number, width)) {
                return isSigned ? TValue{target, static_cast<std::int64_t>(*number)} : TValue{target, static_cast<std::uint64_t>(*number)};
            }
        }
    }
    return NullValue(target);
}

void ValidateValue(const TValue& value, TSourcePosition position) {
    ValidateType(value.Type, position);
    if (value.IsNull()) {
        if (!value.Type.Nullable && value.Type.Kind != K::Null) {
            Fail(position, "NULL requires an optional type");
        }
        return;
    }
    bool valid = false;
    if (value.Type.IsSigned() || value.Type.Kind == K::Interval) {
        if (const auto* number = std::get_if<std::int64_t>(&value.Data)) {
            valid = value.Type.Kind == K::Interval ? FitsTemporal(*number, K::Interval) : FitsSigned(*number, Width(value.Type.Kind));
        }
    } else if (value.Type.Kind == K::Calendar) {
        const auto* micros = std::get_if<std::uint64_t>(&value.Data);
        valid = micros && *micros < TimestampLimit;
    } else if (value.Type.IsIntegral() || IsDate(value.Type.Kind)) {
        if (const auto* number = std::get_if<std::uint64_t>(&value.Data)) {
            const int width = value.Type.Kind == K::Date ? 16 : value.Type.Kind == K::Datetime ? 32 :
                value.Type.Kind == K::Timestamp ? 64 : Width(value.Type.Kind);
            valid = IsDate(value.Type.Kind) ? FitsTemporal(*number, value.Type.Kind) : FitsUnsigned(*number, width);
        }
    } else if (value.Type.IsString()) {
        valid = std::holds_alternative<std::string>(value.Data);
        if (valid && value.Type.Kind == K::Utf8) valid = IsUtf8(std::get<std::string>(value.Data));
    } else if (value.Type.Kind == K::Bool) {
        valid = std::holds_alternative<bool>(value.Data);
    } else if (value.Type.Kind == K::Double || value.Type.Kind == K::Float) {
        valid = std::holds_alternative<double>(value.Data);
    } else if (value.Type.Kind == K::Decimal) {
        valid = std::holds_alternative<TDecimal>(value.Data) && (FitsDecimal(std::get<TDecimal>(value.Data).Coefficient, value.Type) ||
            IsDecimalSpecial(std::get<TDecimal>(value.Data).Coefficient));
    }
    if (!valid) {
        Fail(position, "Value does not match " + value.Type.ToString());
    }
}

} // namespace

TFrontendError::TFrontendError(TSourcePosition position, std::string message)
    : std::runtime_error(std::move(message)), Position(position)
{}

TType CommonType(TType left, TType right, TSourcePosition position) {
    return ComputeCommonType(left, right, position);
}

TType TType::Optional() const {
    auto result = *this;
    result.Nullable = true;
    return result;
}

TType TType::Required() const {
    auto result = *this;
    result.Nullable = false;
    return result;
}

bool TType::IsIntegral() const { return Width(Kind) != 0; }
bool TType::IsNumeric() const { return IsIntegral() || Kind == K::Float || Kind == K::Double || Kind == K::Decimal; }
bool TType::IsString() const { return Kind == K::String || Kind == K::Utf8; }
bool TType::IsSigned() const { return Kind >= K::Int8 && Kind <= K::Int64; }

std::string TType::ToString() const {
    static constexpr const char* Names[] = {"Null", "Bool", "Int8", "Int16", "Int32", "Int64", "Uint8", "Uint16", "Uint32", "Uint64",
        "Float", "Double", "String", "Utf8", "Decimal", "Date", "Datetime", "Timestamp", "Interval", "Calendar"};
    std::string result = Names[static_cast<unsigned>(Kind)];
    if (Kind == K::Decimal) {
        result += "(" + std::to_string(Precision) + "," + std::to_string(Scale) + ")";
    }
    return result + (Nullable ? "?" : "");
}

TType ParseType(std::string_view name, TSourcePosition position) {
    bool nullable = !name.empty() && name.back() == '?';
    if (nullable) {
        name.remove_suffix(1);
    }
    const auto normalized = Lower(name);
    for (unsigned i = 0; i <= static_cast<unsigned>(K::Interval); ++i) {
        TType type{static_cast<K>(i), nullable};
        if (Lower(type.ToString().substr(0, type.ToString().find('?'))) == normalized && type.Kind != K::Decimal) {
            return type;
        }
    }
    if (normalized.starts_with("decimal(") && normalized.back() == ')') {
        const auto comma = normalized.find(',');
        if (comma != std::string::npos) {
            unsigned precision = 0, scale = 0;
            const auto a = std::from_chars(normalized.data() + 8, normalized.data() + comma, precision);
            const auto b = std::from_chars(normalized.data() + comma + 1, normalized.data() + normalized.size() - 1, scale);
            if (a.ec == std::errc{} && b.ec == std::errc{} && a.ptr == normalized.data() + comma &&
                b.ptr == normalized.data() + normalized.size() - 1 && precision <= 35 && scale <= precision) {
                TType type{K::Decimal, nullable, static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale)};
                ValidateType(type, position);
                return type;
            }
        }
    }
    throw TUnsupportedFeature(EFeature::Type, position, "Unsupported scalar type: " + std::string(name));
}

const TExprNode& TExpressionArena::operator[](TExprId id) const {
    if (id >= Nodes_.size()) {
        throw std::out_of_range("Invalid native expression ID");
    }
    return Nodes_[id];
}

TExprId TExpressionArena::Append(TExprNode node) {
    if (Nodes_.size() == std::numeric_limits<TExprId>::max()) {
        Fail(node.Position, "Too many expressions");
    }
    for (auto arg : node.Arguments) {
        if (arg >= Nodes_.size()) {
            Fail(node.Position, "Expression arguments must precede their consumer");
        }
    }
    const auto id = static_cast<TExprId>(Nodes_.size());
    Nodes_.push_back(std::move(node));
    return id;
}

TExprId TExpressionArena::Literal(TType type, TScalar value, TSourcePosition position) {
    ValidateValue({type, value}, position);
    if (type.Kind == K::Float && std::holds_alternative<double>(value)) {
        value = static_cast<double>(static_cast<float>(std::get<double>(value)));
    }
    return Append({O::Literal, type, position, {}, std::move(value), {}, 0});
}

TExprId TExpressionArena::Null(TSourcePosition position) {
    return Literal({K::Null, true}, std::monostate{}, position);
}

TExprId TExpressionArena::Column(TColumnId column, TType type, TSourcePosition position) {
    ValidateType(type, position);
    return Append({O::Column, type, position, {}, {}, {}, column});
}

TExprId TExpressionArena::Parameter(std::string name, TType type, TSourcePosition position) {
    ValidateType(type, position);
    return Append({O::Parameter, type, position, {}, {}, std::move(name), 0});
}

TExprId TExpressionArena::Convert(TExprId input, TType target, TSourcePosition position) {
    ValidateType(target, position);
    if ((*this)[input].Type == target) {
        return input;
    }
    if (CommonType((*this)[input].Type, target, position) != target) {
        Fail(position, "Implicit conversion cannot narrow " + (*this)[input].Type.ToString() + " to " + target.ToString());
    }
    return Append({O::Convert, target, position, {input}, {}, {}, 0});
}

TExprId TExpressionArena::Cast(TExprId input, TType target, TSourcePosition position) {
    ValidateType(target, position);
    const auto source = (*this)[input].Type;
    if (source.Kind == K::Calendar || target.Kind == K::Calendar)
        throw TUnsupportedFeature(EFeature::Type, position, "calendar values require DateTime accessors or constructors");
    if (target.Kind == K::Null) {
        Fail(position, "Cannot cast to Null");
    }
    if (source.Kind == K::Float && target.IsIntegral() && Width(target.Kind) >= 32) {
        throw TUnsupportedFeature(EFeature::Expression, position,
            "Float to 32/64-bit integers is outside the native profile pending runtime boundary semantics");
    }
    if (target.IsString() && (source.Kind == K::Float || source.Kind == K::Double)) {
        throw TUnsupportedFeature(EFeature::Expression, position, "Floating point to string formatting is not supported");
    }
    if (source.Kind == K::Decimal && target.IsIntegral() && source.Precision == source.Scale)
        Fail(position, "Decimal to integer cast requires integral digits");
    if (target.Kind == K::Decimal && ((source.IsIntegral() && target.Precision == target.Scale) ||
        (source.Kind == K::Decimal && !std::min(source.Precision - source.Scale, target.Precision - target.Scale) &&
            !std::min(source.Scale, target.Scale)))) {
        Fail(position, "Decimal cast requires overlapping integral or fractional digits");
    }
    if (source.Kind != K::Null && source.Required() != target.Required() && !source.IsString() && !target.IsString()) {
        const bool sourceDate = IsDate(source.Kind), targetDate = IsDate(target.Kind);
        const bool sourceFloat = source.Kind == K::Float || source.Kind == K::Double;
        const bool targetFloat = target.Kind == K::Float || target.Kind == K::Double;
        const bool allowed = (source.IsIntegral() && (target.IsNumeric() || target.Kind == K::Bool || targetDate || target.Kind == K::Interval)) ||
            (sourceFloat && (target.IsIntegral() || targetFloat || target.Kind == K::Bool)) ||
            (source.Kind == K::Bool && (target.IsIntegral() || targetFloat)) ||
            (source.Kind == K::Decimal && (target.IsNumeric())) ||
            (sourceDate && (targetDate || target.IsIntegral() || targetFloat)) ||
            (source.Kind == K::Interval && (target.IsIntegral() || targetFloat));
        if (!allowed) Fail(position, "Unsupported cast from " + source.ToString() + " to " + target.ToString());
    }
    if (source.Required() == target.Required()) {
        target.Nullable |= source.Nullable;
        return Convert(input, target, position);
    }
    if (source.IsIntegral() && target.IsIntegral() &&
        ((source.IsSigned() == target.IsSigned() && Width(source.Kind) <= Width(target.Kind)) ||
         (!source.IsSigned() && target.IsSigned() && Width(source.Kind) < Width(target.Kind)))) {
        target.Nullable |= source.Nullable;
        return Convert(input, target, position);
    }
    if (source.IsString() && !target.IsString()) {
        const auto& node = (*this)[input];
        if (node.Op != O::Literal) {
            if (target.Kind == K::Date) return Append({O::Cast, target.Optional(), position, {input}, {}, {}, 0});
            throw TUnsupportedFeature(EFeature::Expression, position, "Text conversion to " + target.ToString() + " requires a literal; dynamic text parsing is not supported");
        }
        if (target.Kind == K::Decimal && std::holds_alternative<std::string>(node.Value) &&
            !IsCanonicalDecimalText(std::get<std::string>(node.Value)))
            throw TUnsupportedFeature(EFeature::Expression, position, "native Decimal text requires canonical digits or inf/nan; exponent and malformed text parsing are not supported");
        // Literal text is converted once with the native lexical contract.
        // Runtime Date parsing uses this same parser in reference execution.
        auto converted = ConvertValue({source, node.Value}, target.Optional());
        return Literal(converted.Type, std::move(converted.Data), position);
    }
    // A potentially lossy conversion is not necessarily nullable: integer to
    // float, float narrowing and integral truth conversion are total in YQL.
    const bool sourceFloat = source.Kind == K::Float || source.Kind == K::Double;
    const bool targetFloat = target.Kind == K::Float || target.Kind == K::Double;
    const bool total =
        (IsDate(source.Kind) && IsDate(target.Kind)) ||
        (source.IsIntegral() && !source.IsSigned() && IsDate(target.Kind) &&
            FitsTemporal((static_cast<__int128>(1) << Width(source.Kind)) - 1, target.Kind)) ||
        (targetFloat && (source.IsIntegral() || sourceFloat || source.Kind == K::Bool || source.Kind == K::Decimal)) ||
        (target.Kind == K::Decimal && (source.IsIntegral() || source.Kind == K::Decimal)) ||
        (source.Kind == K::Bool && target.IsIntegral()) ||
        (target.Kind == K::Bool && source.IsIntegral()) ||
        (target.IsString() && (source.IsIntegral() || source.Kind == K::Bool || source.Kind == K::Utf8 || source.Kind == K::Decimal));
    target.Nullable |= source.Nullable || !total;
    return Append({O::Cast, target, position, {input}, {}, {}, 0});
}

TExprId TExpressionArena::Unary(O op, TExprId input, TSourcePosition position) {
    auto type = (*this)[input].Type;
    if (op == O::Unwrap) {
        if (type.Kind == K::Null) Fail(position, "Unwrap requires a typed value");
        if (!type.Nullable) return input;
        type = type.Required();
    } else if (op == O::CalendarSplit) {
        if (!IsDate(type.Kind)) Fail(position, "DateTime::Split requires Date, Datetime or Timestamp");
        type = {K::Calendar, type.Nullable};
    } else if (op == O::CalendarDate || op == O::CalendarDatetime || op == O::CalendarTimestamp || op == O::CalendarYear) {
        if (type.Kind != K::Calendar) Fail(position, "calendar operation requires a calendar value");
        type = {op == O::CalendarDate ? K::Date : op == O::CalendarDatetime ? K::Datetime : op == O::CalendarTimestamp ? K::Timestamp : K::Uint16, type.Nullable};
    } else if (op == O::StablePickle) {
        if (type.Kind == K::Calendar) throw TUnsupportedFeature(EFeature::Type, position, "calendar values cannot be persisted");
        type = {K::String};
    } else if (op == O::IsNull || op == O::IsNotNull) {
        type = {K::Bool};
    } else if (op == O::Not) {
        if (type.Kind != K::Bool && type.Kind != K::Null) {
            Fail(position, "NOT expects Bool");
        }
        type = {K::Bool, type.Nullable || type.Kind == K::Null};
        input = Convert(input, type, position);
    } else if (op == O::Negate || op == O::Abs) {
        if (!type.IsNumeric() && type.Kind != K::Interval) {
            Fail(position, "Unary arithmetic expects a number");
        }
    } else if (op == O::Length || op == O::Lower || op == O::Upper) {
        if (!type.IsString()) {
            Fail(position, "String function expects String or Utf8");
        }
        if (op == O::Length) {
            type = {K::Uint32, type.Nullable};
        }
    } else {
        Fail(position, "Unsupported unary operation");
    }
    return Append({op, type, position, {input}, {}, {}, 0});
}

TExprId TExpressionArena::Binary(O op, TExprId left, TExprId right, TSourcePosition position) {
    const auto leftType = (*this)[left].Type;
    const auto rightType = (*this)[right].Type;
    if (op == O::CalendarShiftMonths || op == O::CalendarShiftYears) {
        if (leftType.Kind != K::Calendar || rightType != TType{K::Int32})
            Fail(position, "calendar shift requires a calendar value and non-null Int32 amount");
        return Append({op, {K::Calendar, true}, position, {left, right}, {}, {}, 0});
    }
    if (leftType.Kind == K::Calendar || rightType.Kind == K::Calendar)
        throw TUnsupportedFeature(EFeature::Type, position, "calendar values require DateTime operations");
    if ((op == O::Add || op == O::Subtract) && IsDate(leftType.Kind) && rightType.Kind == K::Interval) {
        return Append({op, leftType.Optional(), position, {left, right}, {}, {}, 0});
    }
    if (op == O::Add && leftType.Kind == K::Interval && IsDate(rightType.Kind)) {
        return Append({op, rightType.Optional(), position, {right, left}, {}, {}, 0});
    }
    if (op == O::Subtract && IsDate(leftType.Kind) && IsDate(rightType.Kind)) {
        return Append({op, {K::Interval, true}, position, {left, right}, {}, {}, 0});
    }
    if (op == O::Multiply && ((leftType.Kind == K::Decimal && rightType.IsIntegral()) ||
        (rightType.Kind == K::Decimal && leftType.IsIntegral()))) {
        // An integer factor does not change Decimal precision or scale. Keep
        // it integral: converting a large factor first can turn 0 * n into NaN.
        auto result = leftType.Kind == K::Decimal ? leftType : rightType;
        result.Nullable = leftType.Nullable || rightType.Nullable;
        if (leftType.IsIntegral()) std::swap(left, right);
        return Append({op, result, position, {left, right}, {}, {}, 0});
    }
    auto common = CommonType(leftType, rightType, position);
    auto result = common;
    if (op == O::And || op == O::Or) {
        if (common.Kind != K::Bool && common.Kind != K::Null) {
            Fail(position, "Boolean operation expects Bool operands");
        }
        common = result = {K::Bool, common.Nullable || common.Kind == K::Null};
    } else if (IsComparison(op)) {
        result = {K::Bool, common.Nullable || common.Kind == K::Null};
    } else if (op == O::Concat || op == O::Contains || op == O::StartsWith || op == O::EndsWith) {
        if (!common.IsString()) {
            Fail(position, "String operation expects string operands");
        }
        result = op == O::Concat ? common : TType{K::Bool, common.Nullable};
    } else if (op == O::Add || op == O::Subtract || op == O::Multiply || op == O::Divide || op == O::Modulo) {
        if (!common.IsNumeric() && !(common.Kind == K::Interval && (op == O::Add || op == O::Subtract))) {
            Fail(position, "Arithmetic expects numeric operands");
        }
        if (op == O::Modulo && !common.IsIntegral() && common.Kind != K::Decimal) {
            Fail(position, "Modulo expects integral operands");
        }
        if ((op == O::Modulo || op == O::Divide) && common.IsIntegral()) {
            result.Nullable = true;
        }
        if (common.Kind == K::Interval) result.Nullable = true;
    } else {
        Fail(position, "Unsupported binary operation");
    }
    left = Convert(left, common, position);
    right = Convert(right, common, position);
    return Append({op, result, position, {left, right}, {}, {}, 0});
}

TExprId TExpressionArena::If(TExprId condition, TExprId thenExpr, TExprId elseExpr, TSourcePosition position) {
    const auto conditionType = (*this)[condition].Type;
    if (conditionType.Kind != K::Bool && conditionType.Kind != K::Null) {
        Fail(position, "IF condition expects Bool");
    }
    condition = Convert(condition, {K::Bool, conditionType.Nullable || conditionType.Kind == K::Null}, position);
    const auto type = CommonType((*this)[thenExpr].Type, (*this)[elseExpr].Type, position);
    thenExpr = Convert(thenExpr, type, position);
    elseExpr = Convert(elseExpr, type, position);
    return Append({O::If, type, position, {condition, thenExpr, elseExpr}, {}, {}, 0});
}

TExprId TExpressionArena::Coalesce(std::span<const TExprId> arguments, TSourcePosition position) {
    if (arguments.empty()) {
        Fail(position, "COALESCE requires at least one argument");
    }
    auto type = (*this)[arguments.front()].Type;
    bool nullable = true;
    for (auto id : arguments) {
        const auto current = (*this)[id].Type;
        type = CommonType(type, current, position);
        nullable &= current.Nullable || current.Kind == K::Null;
    }
    std::vector<TExprId> converted;
    converted.reserve(arguments.size());
    for (auto id : arguments) {
        auto target = type;
        const auto input = (*this)[id].Type;
        // Keep each alternative's nullability: lifting a required fallback
        // here loses that guarantee when a rewrite reconstructs Coalesce.
        target.Nullable = input.Nullable || input.Kind == K::Null;
        converted.push_back(Convert(id, target, position));
    }
    type.Nullable = nullable;
    return Append({O::Coalesce, type, position, std::move(converted), {}, {}, 0});
}

TExprId TExpressionArena::Ensure(TExprId value, TExprId predicate, std::string message, TSourcePosition position) {
    if ((*this)[predicate].Type != TType{K::Bool}) Fail(position, "Ensure requires a non-null Boolean predicate");
    return Append({O::Ensure, (*this)[value].Type, position, {value, predicate}, {}, std::move(message), 0});
}

TExprId TExpressionArena::Find(TExprId input, TExprId needle, TExprId offset, TSourcePosition position) {
    const auto type = (*this)[input].Type;
    if (!type.IsString() || (*this)[needle].Type != type.Required() || (*this)[offset].Type != TType{K::Uint32})
        Fail(position, "Find requires matching string types, a non-null needle and a non-null Uint32 offset");
    return Append({O::Find, {K::Uint32, true}, position, {input, needle, offset}, {}, {}, 0});
}

TExprId TExpressionArena::Call(std::string_view name, std::span<const TExprId> arguments, TSourcePosition position) {
    const auto lowered = Lower(name);
    auto arity = [&](std::size_t expected) {
        if (arguments.size() != expected) {
            Fail(position, std::string(name) + " expects " + std::to_string(expected) + " arguments");
        }
    };
    if (lowered.starts_with("datetime::") || lowered.starts_with("datetime2::")) {
        const auto function = lowered.substr(lowered.find("::") + 2);
        if (function == "intervalfromdays" || function == "intervalfromhours" || function == "intervalfromminutes") {
            arity(1);
            auto amount = arguments[0];
            const auto value = (*this)[amount];
            if (value.Type.Required() != TType{K::Int32}) {
                if (value.Op != O::Literal || !value.Type.IsIntegral() || value.Type.Nullable)
                    throw TUnsupportedFeature(EFeature::Type, position, "native interval constructors require Int32 amounts or fitting integer literals");
                const auto number = IntegralValue({value.Type, value.Value});
                if (!number || !FitsSigned(*number, 32)) Fail(position, "interval amount must fit Int32");
                amount = Literal({K::Int32}, static_cast<std::int64_t>(*number), position);
            }
            const auto scale = function == "intervalfromdays" ? MicrosPerDay :
                function == "intervalfromhours" ? 3'600'000'000LL : 60'000'000LL;
            const auto wide = Convert(amount, {K::Int64, (*this)[amount].Type.Nullable}, position);
            const auto factor = Literal({K::Int64}, static_cast<std::int64_t>(scale), position);
            // Runtime constructors multiply in their signed 64-bit layout,
            // then check the narrow interval range. Ordinary native operations
            // preserve that wrapping and NULL behavior without a resource/UDF.
            return Cast(Binary(O::Multiply, wide, factor, position), {K::Interval}, position);
        }
        auto calendar = [&](TExprId value) {
            if ((*this)[value].Type.Kind == K::Calendar) return value;
            return Unary(O::CalendarSplit, value, position);
        };
        if (function == "split") { arity(1); return Unary(O::CalendarSplit, arguments[0], position); }
        if (function == "shiftmonths" || function == "shiftyears") {
            arity(2);
            auto amount = arguments[1];
            const auto value = (*this)[amount];
            if (value.Type != TType{K::Int32}) {
                if (value.Op != O::Literal || !value.Type.IsIntegral() || value.Type.Nullable)
                    throw TUnsupportedFeature(EFeature::Type, position, "native calendar shifts require Int32 amounts or fitting integer literals");
                const auto number = IntegralValue({value.Type, value.Value});
                if (!number || !FitsSigned(*number, 32)) Fail(position, "calendar shift amount must fit Int32");
                amount = Literal({K::Int32}, static_cast<std::int64_t>(*number), position);
            }
            return Binary(function == "shiftmonths" ? O::CalendarShiftMonths : O::CalendarShiftYears, calendar(arguments[0]), amount, position);
        }
        for (const auto& [functionName, op] : {std::pair{"makedate", O::CalendarDate}, {"makedatetime", O::CalendarDatetime},
            {"maketimestamp", O::CalendarTimestamp}, {"getyear", O::CalendarYear}}) {
            if (function == functionName) { arity(1); return Unary(op, calendar(arguments[0]), position); }
        }
    }
    if (lowered == "decimal") {
        arity(3);
        const auto text = (*this)[arguments[0]];
        const auto precision = (*this)[arguments[1]], scale = (*this)[arguments[2]];
        if (text.Op != O::Literal || !text.Type.IsString() || text.Type.Nullable ||
            precision.Op != O::Literal || !precision.Type.IsIntegral() || precision.Type.Nullable ||
            scale.Op != O::Literal || !scale.Type.IsIntegral() || scale.Type.Nullable)
            throw TUnsupportedFeature(EFeature::Expression, position, "Decimal constructor requires literal text, precision and scale");
        const auto p = *IntegralValue({precision.Type, precision.Value}), s = *IntegralValue({scale.Type, scale.Value});
        if (p < 1 || p > 35 || s < 0 || s > p) Fail(position, "Decimal requires 1 <= precision <= 35 and scale <= precision");
        const auto& value = std::get<std::string>(text.Value);
        if (!IsCanonicalDecimalText(value))
            throw TUnsupportedFeature(EFeature::Expression, position, "native Decimal text requires canonical digits or inf/nan");
        const TType type{K::Decimal, false, static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(s)};
        return Literal(type, TDecimal{*ParseDecimal(value, type)}, position);
    }
    if (lowered == "coalesce") {
        return Coalesce(arguments, position);
    }
    if (lowered == "if") {
        if (arguments.size() == 2) {
            const auto condition = arguments[0], thenExpr = arguments[1];
            return If(condition, thenExpr, Null(position), position);
        }
        arity(3);
        return If(arguments[0], arguments[1], arguments[2], position);
    }
    for (const auto& [function, op] : {std::pair{"abs", O::Abs}, {"length", O::Length}, {"lower", O::Lower}, {"upper", O::Upper},
        {"string::tolower", O::Lower}, {"string::toupper", O::Upper},
        {"string::asciitolower", O::Lower}, {"string::asciitoupper", O::Upper}}) {
        if (lowered == function) {
            arity(1);
            return Unary(op, arguments[0], position);
        }
    }
    for (const auto& [function, op] : {std::pair{"contains", O::Contains}, {"startswith", O::StartsWith}, {"endswith", O::EndsWith},
        {"string::contains", O::Contains}, {"string::startswith", O::StartsWith}, {"string::endswith", O::EndsWith}, {"concat", O::Concat}}) {
        if (lowered == function) {
            arity(2);
            return Binary(op, arguments[0], arguments[1], position);
        }
    }
    if (lowered == "substring" || lowered == "substr") {
        if (arguments.size() != 2 && arguments.size() != 3) {
            Fail(position, "Substring expects two or three arguments");
        }
        const auto type = (*this)[arguments[0]].Type;
        if (type.Kind == K::Null) return arguments[0];
        if (type.Kind != K::String) Fail(position, "Substring expects String");
        std::vector<TExprId> args{arguments[0]};
        for (std::size_t i = 1; i != 3; ++i) {
            const std::uint64_t defaultValue = i == 1 ? 0 : std::numeric_limits<std::uint32_t>::max();
            if (i == arguments.size()) {
                args.push_back(Literal({K::Uint32}, defaultValue, position));
                continue;
            }
            const auto input = arguments[i];
            const auto node = (*this)[input];
            if (node.Type.Kind == K::Null || (node.Op == O::Literal && std::holds_alternative<std::monostate>(node.Value))) {
                args.push_back(Literal({K::Uint32}, defaultValue, position));
            } else if (node.Op == O::Literal && node.Type.IsIntegral()) {
                const auto number = IntegralValue({node.Type, node.Value});
                if (!number || !FitsUnsigned(*number, 32)) Fail(position, "Substring offset/length must fit Uint32");
                args.push_back(Literal({K::Uint32}, static_cast<std::uint64_t>(*number), position));
            } else if (node.Type.Kind == K::Uint8 || node.Type.Kind == K::Uint16 || node.Type.Kind == K::Uint32) {
                auto value = Convert(input, {K::Uint32, node.Type.Nullable}, position);
                if (node.Type.Nullable) {
                    const TExprId alternatives[]{value, Literal({K::Uint32}, defaultValue, position)};
                    value = Coalesce(alternatives, position);
                }
                args.push_back(value);
            } else if (node.Type.IsIntegral()) {
                throw TUnsupportedFeature(EFeature::Expression, position, "dynamic Substring offsets require Uint8/16/32");
            } else Fail(position, "Substring offsets must be integral");
        }
        return Append({O::Substring, type, position, std::move(args), {}, {}, 0});
    }
    std::optional<TType> castType;
    try {
        castType = ParseType(name, position);
    } catch (const TFrontendError&) {
    }
    if (castType && castType->Kind != K::Null) {
        arity(1);
        const auto source = (*this)[arguments[0]];
        if (source.Op != O::Literal || !source.Type.IsString())
            throw TUnsupportedFeature(EFeature::Expression, position, "native data constructors require a string literal");
        auto value = ConvertValue({source.Type, source.Value}, castType->Optional());
        if (value.IsNull()) Fail(position, "Invalid " + castType->ToString() + " literal");
        return Literal(castType->Required(), std::move(value.Data), position);
    }
    throw TUnsupportedFeature(EFeature::Function, position, "Unsupported scalar function: " + std::string(name));
}

TExprId TExpressionArena::Rebuild(TExprNode node) {
    switch (node.Op) {
        case O::Literal: return Literal(node.Type, std::move(node.Value), node.Position);
        case O::Column: return Column(node.ColumnId, node.Type, node.Position);
        case O::Parameter: return Parameter(std::move(node.Name), node.Type, node.Position);
        case O::Convert:
            return Convert(node.Arguments[0], CommonType(node.Type, (*this)[node.Arguments[0]].Type, node.Position), node.Position);
        case O::Cast: return Cast(node.Arguments[0], node.Type, node.Position);
        case O::If: return If(node.Arguments[0], node.Arguments[1], node.Arguments[2], node.Position);
        case O::Coalesce: return Coalesce(node.Arguments, node.Position);
        case O::Substring: return Call("Substring", node.Arguments, node.Position);
        case O::Ensure: return Ensure(node.Arguments[0], node.Arguments[1], std::move(node.Name), node.Position);
        case O::Find: return Find(node.Arguments[0], node.Arguments[1], node.Arguments[2], node.Position);
        default:
            return node.Arguments.size() == 1 ? Unary(node.Op, node.Arguments[0], node.Position) :
                Binary(node.Op, node.Arguments[0], node.Arguments[1], node.Position);
    }
}

TExprId TExpressionArena::Import(const TExpressionArena& source, TExprId root,
    const std::function<TExprId(TColumnId, TType, TSourcePosition)>& bindColumn) {
    std::unordered_map<TExprId, TExprId> copied;
    std::vector<std::pair<TExprId, bool>> stack{{root, false}};
    while (!stack.empty()) {
        const auto [id, ready] = stack.back();
        stack.pop_back();
        if (copied.contains(id)) {
            continue;
        }
        // Copy because importing into this same arena may reallocate its storage.
        auto node = source[id];
        if (!ready) {
            stack.emplace_back(id, true);
            for (auto it = node.Arguments.rbegin(); it != node.Arguments.rend(); ++it) {
                if (!copied.contains(*it)) {
                    stack.emplace_back(*it, false);
                }
            }
            continue;
        }
        for (auto& arg : node.Arguments) {
            arg = copied.at(arg);
        }
        const auto result = node.Op == O::Column ? bindColumn(node.ColumnId, node.Type, node.Position) : Rebuild(std::move(node));
        (void)(*this)[result];
        copied.emplace(id, result);
    }
    return copied.at(root);
}

TExprId TExpressionArena::RewriteColumns(TExprId root,
    const std::function<TExprId(TExprId, TColumnId, TType, TSourcePosition)>& bindColumn) {
    std::unordered_map<TExprId, TExprId> rewritten;
    std::vector<std::pair<TExprId, bool>> stack{{root, false}};
    while (!stack.empty()) {
        const auto [id, ready] = stack.back();
        stack.pop_back();
        if (rewritten.contains(id)) continue;
        // The callback and rebuilding may append and reallocate this arena.
        auto node = (*this)[id];
        if (node.Op == O::Column) {
            const auto replacement = bindColumn(id, node.ColumnId, node.Type, node.Position);
            (void)(*this)[replacement];
            rewritten.emplace(id, replacement);
        } else if (!ready && !node.Arguments.empty()) {
            stack.emplace_back(id, true);
            for (auto it = node.Arguments.rbegin(); it != node.Arguments.rend(); ++it) {
                if (!rewritten.contains(*it)) stack.emplace_back(*it, false);
            }
        } else {
            bool changed = false;
            for (auto& argument : node.Arguments) {
                const auto replacement = rewritten.at(argument);
                changed |= replacement != argument;
                argument = replacement;
            }
            rewritten.emplace(id, changed ? Rebuild(std::move(node)) : id);
        }
    }
    return rewritten.at(root);
}

std::string FormatValue(const TValue& value) {
    if (value.IsNull()) {
        return "NULL";
    }
    if (auto p = std::get_if<std::string>(&value.Data)) {
        return *p;
    }
    if (auto p = std::get_if<bool>(&value.Data)) {
        return *p ? "true" : "false";
    }
    if (auto p = std::get_if<std::int64_t>(&value.Data)) {
        return std::to_string(*p);
    }
    if (auto p = std::get_if<std::uint64_t>(&value.Data)) {
        return std::to_string(*p);
    }
    if (auto p = std::get_if<double>(&value.Data)) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(std::numeric_limits<double>::max_digits10);
        out << *p;
        return out.str();
    }
    const auto coefficient = std::get<TDecimal>(value.Data).Coefficient;
    if (coefficient == TDecimal::NaN()) return "nan";
    if (coefficient == TDecimal::Inf()) return "inf";
    if (coefficient == -TDecimal::Inf()) return "-inf";
    auto text = Int128ToString(coefficient < 0 ? -coefficient : coefficient);
    if (value.Type.Scale) {
        if (text.size() <= value.Type.Scale) {
            text.insert(std::size_t{0}, value.Type.Scale + 1 - text.size(), '0');
        }
        text.insert(text.size() - value.Type.Scale, 1, '.');
    }
    return (coefficient < 0 ? "-" : "") + text;
}

namespace {

// The scalar subset of the backend's stable encoding. Correlation domains use
// these bytes as null-safe keys. Keep this executable without a YQL dependency;
// backend lowering calls StablePickle directly, and parity tests check the wire
// format for every native type, including NaNs, signed zero and NULL.
std::string StablePickle(const TValue& value) {
    std::string payload;
    auto littleEndian = [&](std::uint64_t bits, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i, bits >>= 8) payload.push_back(static_cast<char>(bits));
    };
    auto packedInteger = [&](std::uint64_t bits, unsigned width) {
        const unsigned maxBytes = width <= 32 ? 3 : 7;
        for (unsigned bytes = 1; bytes <= maxBytes; ++bytes) {
            if (bits < (std::uint64_t{1} << (7 * bytes))) {
                littleEndian((bits << bytes) | ((std::uint64_t{1} << (bytes - 1)) - 1), bytes);
                return;
            }
        }
        payload.push_back(static_cast<char>((1u << maxBytes) - 1));
        littleEndian(bits, width <= 32 ? 4 : 8);
    };
    if (!value.IsNull()) {
        const auto kind = value.Type.Kind;
        if (kind == K::Bool) {
            payload.push_back(std::get<bool>(value.Data));
        } else if (value.Type.IsIntegral() || IsDate(kind) || kind == K::Interval) {
            const unsigned width = kind == K::Date ? 16 : kind == K::Datetime ? 32 :
                kind == K::Timestamp || kind == K::Interval ? 64 : Width(kind);
            const bool isSigned = value.Type.IsSigned() || kind == K::Interval;
            auto bits = isSigned ? static_cast<std::uint64_t>(std::get<std::int64_t>(value.Data)) :
                std::get<std::uint64_t>(value.Data);
            if (width == 8) {
                littleEndian(bits, 1);
            } else {
                if (isSigned) bits = (bits << 1) ^ (std::uint64_t{0} - (bits >> 63));
                packedInteger(bits, width);
            }
        } else if (kind == K::Float || kind == K::Double) {
            const auto number = std::get<double>(value.Data);
            if (kind == K::Float) {
                littleEndian(std::isnan(number) ? 0x7fc00000u : number == 0 ? 0 :
                    std::bit_cast<std::uint32_t>(static_cast<float>(number)), 4);
            } else {
                littleEndian(std::isnan(number) ? 0x7ff8000000000000ULL : number == 0 ? 0 :
                    std::bit_cast<std::uint64_t>(number), 8);
            }
        } else if (kind == K::Decimal) {
            const auto coefficient = std::get<TDecimal>(value.Data).Coefficient;
            if (coefficient == TDecimal::NaN()) payload.push_back('\xff');
            else if (coefficient == TDecimal::Inf()) payload.push_back('\xfe');
            else if (coefficient == -TDecimal::Inf()) payload.push_back('\x01');
            else if (coefficient == -TDecimal::NaN()) payload.push_back('\x00');
            else {
                const auto bits = static_cast<unsigned __int128>(coefficient);
                const unsigned signByte = coefficient < 0 ? 0xff : 0;
                unsigned bytes = 15;
                while (bytes && ((bits >> (8 * (bytes - 1))) & 0xff) == signByte) --bytes;
                payload.push_back(static_cast<char>(coefficient < 0 ? 0x7f - bytes : 0x80 + bytes));
                while (bytes) payload.push_back(static_cast<char>(bits >> (8 * --bytes)));
            }
        } else if (value.Type.IsString()) {
            payload = std::get<std::string>(value.Data);
        } else {
            throw std::logic_error("invalid native stable key type");
        }
    }
    const auto size = payload.size();
    if (size > std::numeric_limits<std::uint32_t>::max())
        throw TResourceLimitError({}, "stable scalar encoding exceeds 4 GiB");
    if (size <= 7) {
        const auto empty = value.IsNull() && value.Type.Kind != K::Null;
        payload.insert(payload.begin(), static_cast<char>(1 | (size << 1) | (empty ? 0x10 : 0)));
    } else {
        payload.insert(payload.begin(), 4, '\0');
        for (unsigned i = 0; i < 4; ++i) payload[i] = static_cast<char>(size >> (8 * i));
    }
    return payload;
}

TValue Execute(const TExprNode& node, const std::vector<std::optional<TValue>>& values) {
    auto argument = [&](std::size_t index) -> const TValue& { return *values[node.Arguments[index]]; };
    if (node.Op == O::Unwrap) {
        if (argument(0).IsNull()) Fail(node.Position, "cannot unwrap NULL");
        return {node.Type, argument(0).Data};
    }
    if (node.Op == O::Cast || node.Op == O::Convert) {
        return ConvertValue(argument(0), node.Type);
    }
    if (node.Op == O::IsNull || node.Op == O::IsNotNull) {
        return {node.Type, argument(0).IsNull() == (node.Op == O::IsNull)};
    }
    if (node.Op == O::StablePickle) {
        return {node.Type, StablePickle(argument(0))};
    }
    for (auto id : node.Arguments) {
        if (values[id]->IsNull()) {
            return NullValue(node.Type);
        }
    }
    const auto& left = argument(0);
    if (node.Op == O::CalendarSplit)
        return {node.Type, std::get<std::uint64_t>(left.Data) * DateUnit(left.Type.Kind)};
    if (node.Op == O::CalendarDate || node.Op == O::CalendarDatetime || node.Op == O::CalendarTimestamp)
        return {node.Type, std::get<std::uint64_t>(left.Data) / DateUnit(node.Type.Kind)};
    if (node.Op == O::CalendarYear)
        return {node.Type, static_cast<std::uint64_t>(int(CalendarDate(std::get<std::uint64_t>(left.Data)).year()))};
    if (node.Op == O::CalendarShiftMonths || node.Op == O::CalendarShiftYears) {
        const auto shifted = ShiftCalendar(std::get<std::uint64_t>(left.Data), std::get<std::int64_t>(argument(1).Data), node.Op == O::CalendarShiftYears);
        return shifted ? TValue{node.Type, *shifted} : NullValue(node.Type);
    }
    if (node.Op == O::Not) {
        return {node.Type, !std::get<bool>(left.Data)};
    }
    if (node.Op == O::Length) {
        return {node.Type, static_cast<std::uint64_t>(std::get<std::string>(left.Data).size())};
    }
    if (node.Op == O::Lower || node.Op == O::Upper) {
        auto result = std::get<std::string>(left.Data);
        for (auto& ch : result) {
            if (node.Op == O::Lower && ch >= 'A' && ch <= 'Z') {
                ch += 'a' - 'A';
            } else if (node.Op == O::Upper && ch >= 'a' && ch <= 'z') {
                ch -= 'a' - 'A';
            }
        }
        return {node.Type, std::move(result)};
    }
    if (node.Op == O::Substring) {
        const auto& text = std::get<std::string>(left.Data);
        const auto offset = std::get<std::uint64_t>(argument(1).Data);
        const auto count = node.Arguments.size() == 3 ? std::get<std::uint64_t>(argument(2).Data) : text.size();
        return {node.Type, offset >= text.size() ? std::string{} : text.substr(offset, std::min<std::uint64_t>(count, text.size()))};
    }
    if (node.Op == O::Find) {
        const auto found = std::get<std::string>(left.Data).find(std::get<std::string>(argument(1).Data), std::get<std::uint64_t>(argument(2).Data));
        return found == std::string::npos ? NullValue(node.Type) : TValue{node.Type, static_cast<std::uint64_t>(found)};
    }
    if (node.Op == O::Negate || node.Op == O::Abs) {
        if (auto number = std::get_if<double>(&left.Data)) {
            return {node.Type, node.Op == O::Negate ? -*number : std::abs(*number)};
        }
        __int128 number = left.Type.Kind == K::Decimal ? std::get<TDecimal>(left.Data).Coefficient : *IntegralValue(left);
        if (left.Type.Kind == K::Decimal && number == TDecimal::NaN()) return {node.Type, TDecimal{number}};
        if (node.Op == O::Negate || number < 0) number = -number;
        if (node.Type.Kind == K::Decimal) {
            return {node.Type, TDecimal{number}};
        }
        return {node.Type, WrapInteger(static_cast<unsigned __int128>(number), node.Type)};
    }
    const auto& right = argument(1);
    if ((node.Op == O::Add || node.Op == O::Subtract) && IsDate(left.Type.Kind)) {
        const __int128 a = static_cast<__int128>(std::get<std::uint64_t>(left.Data)) * DateUnit(left.Type.Kind);
        const __int128 b = IsDate(right.Type.Kind) ? static_cast<__int128>(std::get<std::uint64_t>(right.Data)) * DateUnit(right.Type.Kind) :
            std::get<std::int64_t>(right.Data);
        const __int128 result = node.Op == O::Add ? a + b : a - b;
        if (node.Type.Kind == K::Interval) {
            return FitsTemporal(result, K::Interval) ? TValue{node.Type, static_cast<std::int64_t>(result)} : NullValue(node.Type);
        }
        if (result < 0 || result >= TimestampLimit) return NullValue(node.Type);
        return {node.Type, static_cast<std::uint64_t>(result / DateUnit(node.Type.Kind))};
    }
    if (node.Op == O::Concat || node.Op == O::Contains || node.Op == O::StartsWith || node.Op == O::EndsWith) {
        const auto& a = std::get<std::string>(left.Data);
        const auto& b = std::get<std::string>(right.Data);
        if (node.Op == O::Concat) {
            return {node.Type, a + b};
        }
        return {node.Type, node.Op == O::Contains ? a.find(b) != std::string::npos : node.Op == O::StartsWith ? a.starts_with(b) : a.ends_with(b)};
    }
    if (IsComparison(node.Op)) {
        if (left.Type.Kind == K::Decimal && (std::get<TDecimal>(left.Data).Coefficient == TDecimal::NaN() ||
            std::get<TDecimal>(right.Data).Coefficient == TDecimal::NaN())) return {node.Type, node.Op == O::NotEqual};
        int order = 0;
        if (auto a = std::get_if<std::string>(&left.Data)) {
            order = a->compare(std::get<std::string>(right.Data));
        } else if (auto a = std::get_if<double>(&left.Data)) {
            const auto b = std::get<double>(right.Data);
            const bool result = node.Op == O::Equal ? *a == b : node.Op == O::NotEqual ? *a != b :
                node.Op == O::Less ? *a < b : node.Op == O::LessEqual ? *a <= b : node.Op == O::Greater ? *a > b : *a >= b;
            return {node.Type, result};
        } else {
            const auto leftNumber = left.Type.Kind == K::Decimal ? std::get<TDecimal>(left.Data).Coefficient : *IntegralValue(left);
            const auto rightNumber = right.Type.Kind == K::Decimal ? std::get<TDecimal>(right.Data).Coefficient : *IntegralValue(right);
            order = (leftNumber > rightNumber) - (leftNumber < rightNumber);
        }
        const bool result = node.Op == O::Equal ? order == 0 : node.Op == O::NotEqual ? order != 0 :
            node.Op == O::Less ? order < 0 : node.Op == O::LessEqual ? order <= 0 : node.Op == O::Greater ? order > 0 : order >= 0;
        return {node.Type, result};
    }
    if (auto a = std::get_if<double>(&left.Data)) {
        const auto b = std::get<double>(right.Data);
        const auto result = node.Op == O::Add ? *a + b : node.Op == O::Subtract ? *a - b : node.Op == O::Multiply ? *a * b : *a / b;
        return {node.Type, node.Type.Kind == K::Float ? static_cast<double>(static_cast<float>(result)) : result};
    }
    const bool decimal = left.Type.Kind == K::Decimal;
    const __int128 a = decimal ? std::get<TDecimal>(left.Data).Coefficient : *IntegralValue(left);
    const __int128 b = right.Type.Kind == K::Decimal ? std::get<TDecimal>(right.Data).Coefficient : *IntegralValue(right);
    if (!decimal && (node.Op == O::Divide || node.Op == O::Modulo) && b == 0) {
        return NullValue(node.Type);
    }
    if (!decimal) {
        if (node.Type.Kind == K::Interval) {
            const __int128 result = node.Op == O::Add ? a + b : a - b;
            return FitsTemporal(result, K::Interval) ? TValue{node.Type, static_cast<std::int64_t>(result)} : NullValue(node.Type);
        }
        const auto unsignedA = static_cast<unsigned __int128>(a);
        const auto unsignedB = static_cast<unsigned __int128>(b);
        if (node.Op == O::Add || node.Op == O::Subtract || node.Op == O::Multiply) {
            return {node.Type, WrapInteger(node.Op == O::Add ? unsignedA + unsignedB :
                node.Op == O::Subtract ? unsignedA - unsignedB : unsignedA * unsignedB, node.Type)};
        }
        const auto result = node.Op == O::Divide ? a / b : a % b;
        if ((node.Type.IsSigned() || node.Type.Kind == K::Interval) &&
            !FitsSigned(a / b, node.Type.Kind == K::Interval ? 64 : Width(node.Type.Kind))) {
            return NullValue(node.Type);
        }
        return {node.Type, WrapInteger(static_cast<unsigned __int128>(result), node.Type)};
    }
    const auto nan = TDecimal::NaN(), inf = TDecimal::Inf();
    auto decimalResult = [&](const __int128 result) -> TValue { return {node.Type, TDecimal{result}}; };
    if (a == nan || b == nan) return decimalResult(nan);
    const bool specialA = IsDecimalSpecial(a), specialB = IsDecimalSpecial(b);
    const auto signedInf = (a < 0) != (b < 0) ? -inf : inf;
    if (node.Op == O::Add || node.Op == O::Subtract) {
        const auto result = node.Op == O::Add ? a + b : a - b;
        if (specialA || specialB) return decimalResult(!result ? nan : result < 0 ? -inf : inf);
        return decimalResult(BoundDecimal(result, node.Type));
    }
    if (node.Op == O::Multiply) {
        if (specialA || specialB) return decimalResult(!a || !b ? nan : signedInf);
        const auto result = DecimalProduct(a, b, right.Type.IsIntegral() ? 0 : node.Type.Scale);
        return decimalResult(result ? BoundDecimal(*result, node.Type) : signedInf);
    }
    if (node.Op == O::Divide) {
        if (!b) return decimalResult(!a ? nan : a < 0 ? -inf : inf);
        if (specialB) return decimalResult(specialA ? nan : 0);
        if (specialA) return decimalResult(signedInf);
        const auto result = RoundedDecimalQuotient(Int128ToString(a < 0 ? -a : a) + std::string(node.Type.Scale, '0'),
            b < 0 ? -b : b, (a < 0) != (b < 0));
        return decimalResult(result ? BoundDecimal(*result, node.Type) : signedInf);
    }
    if (node.Op == O::Modulo) return decimalResult(!b || specialA || specialB ? nan : a % b);
    Fail(node.Position, "Invalid arithmetic type");
}

} // namespace

TValue Evaluate(const TExpressionArena& arena, TExprId root, const TEvaluationContext& context) {
    // Validate the root before allocating the cache.
    (void)arena[root];
    std::vector<std::optional<TValue>> values(static_cast<std::size_t>(root) + 1);
    struct TFrame {
        TExprId Id;
        std::size_t Next = 0;
    };
    std::vector<TFrame> stack{{root}};
    while (!stack.empty()) {
        const auto id = stack.back().Id;
        const auto& node = arena[id];
        if (values[id]) {
            stack.pop_back();
            continue;
        }
        auto finish = [&](TValue value) {
            value.Type = node.Type;
            if (node.Type.Kind == K::Float && std::holds_alternative<double>(value.Data)) {
                value.Data = static_cast<double>(static_cast<float>(std::get<double>(value.Data)));
            }
            ValidateValue(value, node.Position);
            values[id] = std::move(value);
            stack.pop_back();
        };
        if (node.Op == O::Literal) {
            finish({node.Type, node.Value});
            continue;
        }
        if (node.Op == O::Column || node.Op == O::Parameter) {
            if (node.Op == O::Column ? !context.Column : !context.Parameter) {
                Fail(node.Position, node.Op == O::Column ? "Missing column binding" : "Missing parameter binding: " + node.Name);
            }
            auto value = node.Op == O::Column ? context.Column(node.ColumnId) : context.Parameter(node.Name);
            ValidateValue(value, node.Position);
            if (value.Type.Required() != node.Type.Required() || (value.IsNull() && !node.Type.Nullable)) {
                Fail(node.Position, "Binding type differs from " + node.Type.ToString());
            }
            finish(std::move(value));
            continue;
        }
        if (node.Op == O::Ensure) {
            const auto predicate = node.Arguments[1], value = node.Arguments[0];
            if (!values[predicate]) { stack.push_back({predicate}); continue; }
            if (!std::get<bool>(values[predicate]->Data)) Fail(node.Position, node.Name);
            if (!values[value]) { stack.push_back({value}); continue; }
            finish(*values[value]);
            continue;
        }
        if (node.Op == O::If) {
            const auto condition = node.Arguments[0];
            if (!values[condition]) {
                stack.push_back({condition});
                continue;
            }
            const auto selected = node.Arguments[!values[condition]->IsNull() && std::get<bool>(values[condition]->Data) ? 1 : 2];
            if (!values[selected]) {
                stack.push_back({selected});
                continue;
            }
            finish(*values[selected]);
            continue;
        }
        if (node.Op == O::Coalesce) {
            const auto next = stack.back().Next;
            if (next == node.Arguments.size()) {
                finish(NullValue(node.Type));
                continue;
            }
            const auto arg = node.Arguments[next];
            if (!values[arg]) {
                stack.push_back({arg});
            } else if (!values[arg]->IsNull()) {
                finish(*values[arg]);
            } else {
                ++stack.back().Next;
            }
            continue;
        }
        if (node.Op == O::And || node.Op == O::Or) {
            const auto left = node.Arguments[0], right = node.Arguments[1];
            if (!values[left]) {
                stack.push_back({left});
                continue;
            }
            const bool decisive = node.Op == O::Or;
            if (!values[left]->IsNull() && std::get<bool>(values[left]->Data) == decisive) {
                finish({node.Type, decisive});
                continue;
            }
            if (!values[right]) {
                stack.push_back({right});
                continue;
            }
            if (!values[right]->IsNull() && std::get<bool>(values[right]->Data) == decisive) {
                finish({node.Type, decisive});
            } else if (values[left]->IsNull() || values[right]->IsNull()) {
                finish(NullValue(node.Type));
            } else {
                finish({node.Type, !decisive});
            }
            continue;
        }
        auto& frame = stack.back();
        while (frame.Next < node.Arguments.size() && values[node.Arguments[frame.Next]]) {
            ++frame.Next;
        }
        if (frame.Next < node.Arguments.size()) {
            stack.push_back({node.Arguments[frame.Next]});
            continue;
        }
        finish(Execute(node, values));
    }
    return *values[root];
}

std::vector<TExprId> TExpressionArena::FoldConstants(std::span<const TExprId> roots, std::size_t maxStringBytes) {
    std::unordered_map<TExprId, TExprId> rewritten;
    std::vector<std::pair<TExprId, bool>> pending;
    auto fold = [&](TExprNode node, TExprId original, bool changed) {
        auto retain = [&] { return changed ? Rebuild(node) : original; };
        auto literal = [&](TScalar value) {
            if (const auto* text = std::get_if<std::string>(&value)) {
                if (text->size() > maxStringBytes) return retain();
                maxStringBytes -= text->size();
            }
            return Literal(node.Type, std::move(value), node.Position);
        };
        if (node.Op == O::Literal || node.Op == O::Column || node.Op == O::Parameter) return original;
        if (node.Op == O::If && (*this)[node.Arguments[0]].Op == O::Literal) {
            const auto& condition = (*this)[node.Arguments[0]].Value;
            return node.Arguments[std::holds_alternative<bool>(condition) && std::get<bool>(condition) ? 1 : 2];
        }
        if (node.Op == O::Ensure) {
            const auto& condition = (*this)[node.Arguments[1]];
            // Failed assertions remain runtime errors, even inside dead branches.
            return condition.Op == O::Literal && std::get<bool>(condition.Value) ? node.Arguments[0] : retain();
        }
        if (node.Op == O::Unwrap) {
            const auto& value = (*this)[node.Arguments[0]];
            return value.Op == O::Literal && !std::holds_alternative<std::monostate>(value.Value) ? literal(value.Value) : retain();
        }
        if (node.Op == O::And || node.Op == O::Or) {
            const auto& left = (*this)[node.Arguments[0]];
            const auto& right = (*this)[node.Arguments[1]];
            const bool decisive = node.Op == O::Or;
            if (left.Op != O::Literal) return retain();
            if (std::holds_alternative<bool>(left.Value) && std::get<bool>(left.Value) == decisive) return literal(decisive);
            if (right.Op != O::Literal) return retain();
            if (std::holds_alternative<bool>(right.Value) && std::get<bool>(right.Value) == decisive) return literal(decisive);
            return literal(std::holds_alternative<std::monostate>(left.Value) || std::holds_alternative<std::monostate>(right.Value) ?
                TScalar{std::monostate{}} : TScalar{!decisive});
        }
        if (node.Op == O::Coalesce) {
            for (const auto arg : node.Arguments) {
                const auto& value = (*this)[arg];
                if (value.Op != O::Literal) return retain();
                if (!std::holds_alternative<std::monostate>(value.Value))
                    return value.Type == node.Type ? arg : literal(value.Value);
            }
            return literal(std::monostate{});
        }
        std::size_t inputBytes = 0;
        for (const auto arg : node.Arguments) {
            const auto& value = (*this)[arg];
            if (value.Op != O::Literal) return retain();
            if (const auto* text = std::get_if<std::string>(&value.Value)) {
                if (text->size() > maxStringBytes - inputBytes) return retain();
                inputBytes += text->size();
            }
        }
        // Compact operands avoid allocating a root-sized evaluator cache for
        // every constant operation in a large/shared arena.
        std::vector<std::optional<TValue>> values;
        values.reserve(node.Arguments.size());
        auto executable = node;
        for (auto& arg : executable.Arguments) {
            const auto& value = (*this)[arg];
            values.push_back(TValue{value.Type, value.Value});
            arg = values.size() - 1;
        }
        auto value = Execute(executable, values);
        return literal(std::move(value.Data));
    };
    for (auto root : roots) {
        pending.emplace_back(root, false);
        while (!pending.empty()) {
            const auto [id, ready] = pending.back();
            pending.pop_back();
            if (rewritten.contains(id)) continue;
            auto node = (*this)[id]; // appends may reallocate the arena
            if (!ready && !node.Arguments.empty()) {
                pending.emplace_back(id, true);
                for (auto it = node.Arguments.rbegin(); it != node.Arguments.rend(); ++it)
                    if (!rewritten.contains(*it)) pending.emplace_back(*it, false);
                continue;
            }
            bool changed = false;
            for (auto& arg : node.Arguments) {
                const auto replacement = rewritten.at(arg);
                changed |= replacement != arg;
                arg = replacement;
            }
            rewritten.emplace(id, fold(std::move(node), id, changed));
        }
    }
    std::vector<TExprId> result;
    result.reserve(roots.size());
    for (auto root : roots) result.push_back(rewritten.at(root));
    return result;
}

} // namespace NKikimr::NKqp::NFrontend
