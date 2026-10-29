#include "expression.h"
#include "diagnostic.h"

#include <algorithm>
#include <bit>
#include <charconv>
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
            return {K::Decimal, nullable, 20, 0};
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
    if (text.empty()) {
        return {};
    }
    __int128 result = 0;
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
        if (__builtin_mul_overflow(result, __int128{10}, &result) ||
            __builtin_add_overflow(result, __int128{ch - '0'}, &result)) {
            return {};
        }
        fractional += dot;
    }
    if (!digit || __builtin_mul_overflow(result, Power10(type.Scale - fractional), &result)) {
        return {};
    }
    if (discarded > 5 || (discarded == 5 && (discardedTail || result % 2))) {
        if (__builtin_add_overflow(result, __int128{1}, &result)) return {};
    }
    if (negative) {
        result = -result;
    }
    return FitsDecimal(result, type) ? std::optional<__int128>{result} : std::nullopt;
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
        return RoundedDecimalQuotient(Int128ToString(p->Coefficient < 0 ? -p->Coefficient : p->Coefficient),
            Power10(value.Type.Scale), p->Coefficient < 0);
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
        return static_cast<double>(p->Coefficient) / static_cast<double>(Power10(value.Type.Scale));
    }
    if (auto p = std::get_if<std::string>(&value.Data)) {
        auto special = Lower(*p);
        const bool negative = special.starts_with('-');
        if (special.starts_with('-') || special.starts_with('+')) special.erase(0, 1);
        if (special == "inf" || special == "nan") {
            const double result = special == "inf" ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
            return negative ? -result : result;
        }
        double result;
        std::istringstream input(*p);
        input.imbue(std::locale::classic());
        input >> std::noskipws >> result;
        if (!input.fail() && input.eof() && std::isfinite(result)) {
            return result;
        }
        return {};
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
            if (target.Scale < value.Type.Scale) {
                coefficient = *RoundedDecimalQuotient(Int128ToString(coefficient < 0 ? -coefficient : coefficient),
                    Power10(value.Type.Scale - target.Scale), coefficient < 0);
            } else if (__builtin_mul_overflow(coefficient, Power10(target.Scale - value.Type.Scale), &coefficient)) {
                return NullValue(target);
            }
            if (FitsDecimal(coefficient, target)) {
                return {target, TDecimal{coefficient}};
            }
            return NullValue(target);
        }
        if (auto decimal = ParseDecimal(FormatValue(value), target)) {
            return {target, TDecimal{*decimal}};
        }
        return NullValue(target);
    }
    if (target.Kind == K::Double || target.Kind == K::Float) {
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
        valid = std::holds_alternative<TDecimal>(value.Data) && FitsDecimal(std::get<TDecimal>(value.Data).Coefficient, value.Type);
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
        "Float", "Double", "String", "Utf8", "Decimal", "Date", "Datetime", "Timestamp", "Interval"};
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
    if (target.Kind == K::Null) {
        Fail(position, "Cannot cast to Null");
    }
    if (target.IsString() && (source.Kind == K::Float || source.Kind == K::Double)) {
        throw TUnsupportedFeature(EFeature::Expression, position, "Floating point to string formatting is not supported");
    }
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
            throw TUnsupportedFeature(EFeature::Expression, position, "Text conversion to " + target.ToString() + " requires a literal; dynamic text parsing is not supported");
        }
        // Parse text once with this frontend's explicit lexical contract.
        // The execution model carries only the resulting typed value, so it
        // never depends on the backend's more permissive string parsers.
        auto converted = ConvertValue({source, node.Value}, target.Optional());
        return Literal(converted.Type, std::move(converted.Data), position);
    }
    // Explicit casts are checked execution operations: conversion failure is NULL.
    return Append({O::Cast, target.Optional(), position, {input}, {}, {}, 0});
}

TExprId TExpressionArena::Unary(O op, TExprId input, TSourcePosition position) {
    auto type = (*this)[input].Type;
    if (op == O::IsNull || op == O::IsNotNull) {
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
    if ((op == O::Add || op == O::Subtract) && IsDate(leftType.Kind) && rightType.Kind == K::Interval) {
        return Append({op, leftType.Optional(), position, {left, right}, {}, {}, 0});
    }
    if (op == O::Add && leftType.Kind == K::Interval && IsDate(rightType.Kind)) {
        return Append({op, rightType.Optional(), position, {right, left}, {}, {}, 0});
    }
    if (op == O::Subtract && IsDate(leftType.Kind) && IsDate(rightType.Kind)) {
        return Append({op, {K::Interval, true}, position, {left, right}, {}, {}, 0});
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
        if (op == O::Modulo && !common.IsIntegral()) {
            Fail(position, "Modulo expects integral operands");
        }
        if (op == O::Divide || op == O::Modulo) {
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
        converted.push_back(Convert(id, type, position));
    }
    type.Nullable = nullable;
    return Append({O::Coalesce, type, position, std::move(converted), {}, {}, 0});
}

TExprId TExpressionArena::Call(std::string_view name, std::span<const TExprId> arguments, TSourcePosition position) {
    const auto lowered = Lower(name);
    auto arity = [&](std::size_t expected) {
        if (arguments.size() != expected) {
            Fail(position, std::string(name) + " expects " + std::to_string(expected) + " arguments");
        }
    };
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
        auto type = (*this)[arguments[0]].Type;
        if (!type.IsString()) {
            Fail(position, "Substring expects a string");
        }
        // This primitive slices bytes; an arbitrary slice of UTF-8 need not
        // contain whole code points, so its result is a binary String.
        type.Kind = K::String;
        std::vector<TExprId> args(arguments.begin(), arguments.end());
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto argType = (*this)[args[i]].Type;
            if (!argType.IsIntegral()) {
                Fail(position, "Substring offsets must be integral");
            }
            args[i] = Cast(args[i], {K::Uint64}, position);
            type.Nullable = true;
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

TValue Execute(const TExprNode& node, const std::vector<std::optional<TValue>>& values) {
    auto argument = [&](std::size_t index) -> const TValue& { return *values[node.Arguments[index]]; };
    if (node.Op == O::Cast || node.Op == O::Convert) {
        return ConvertValue(argument(0), node.Type);
    }
    if (node.Op == O::IsNull || node.Op == O::IsNotNull) {
        return {node.Type, argument(0).IsNull() == (node.Op == O::IsNull)};
    }
    for (auto id : node.Arguments) {
        if (values[id]->IsNull()) {
            return NullValue(node.Type);
        }
    }
    const auto& left = argument(0);
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
    if (node.Op == O::Negate || node.Op == O::Abs) {
        if (auto number = std::get_if<double>(&left.Data)) {
            return {node.Type, node.Op == O::Negate ? -*number : std::abs(*number)};
        }
        __int128 number = left.Type.Kind == K::Decimal ? std::get<TDecimal>(left.Data).Coefficient : *IntegralValue(left);
        if (node.Op == O::Negate || number < 0) {
            number = -number;
        }
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
        if (node.Op == O::Divide && b == 0) {
            return NullValue(node.Type);
        }
        const auto result = node.Op == O::Add ? *a + b : node.Op == O::Subtract ? *a - b : node.Op == O::Multiply ? *a * b : *a / b;
        return {node.Type, node.Type.Kind == K::Float ? static_cast<double>(static_cast<float>(result)) : result};
    }
    const bool decimal = left.Type.Kind == K::Decimal;
    const __int128 a = decimal ? std::get<TDecimal>(left.Data).Coefficient : *IntegralValue(left);
    const __int128 b = decimal ? std::get<TDecimal>(right.Data).Coefficient : *IntegralValue(right);
    if ((node.Op == O::Divide || node.Op == O::Modulo) && b == 0) {
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
    __int128 result = 0;
    bool overflow = false;
    switch (node.Op) {
        case O::Add: overflow = __builtin_add_overflow(a, b, &result); break;
        case O::Subtract: overflow = __builtin_sub_overflow(a, b, &result); break;
        case O::Multiply:
            if (const auto product = DecimalProduct(a, b, node.Type.Scale)) result = *product;
            else overflow = true;
            break;
        case O::Divide:
            if (const auto quotient = RoundedDecimalQuotient(Int128ToString(a < 0 ? -a : a) + std::string(node.Type.Scale, '0'),
                b < 0 ? -b : b, (a < 0) != (b < 0))) result = *quotient;
            else overflow = true;
            break;
        case O::Modulo: result = a % b; break;
        default: Fail(node.Position, "Invalid execution opcode");
    }
    if (decimal) {
        if (overflow || !FitsDecimal(result, node.Type)) {
            Fail(node.Position, "Decimal arithmetic overflow");
        }
        return {node.Type, TDecimal{result}};
    }
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

} // namespace NKikimr::NKqp::NFrontend
