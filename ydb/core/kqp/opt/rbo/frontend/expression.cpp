#include "expression.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
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
            continue; // Fixed-scale casts truncate toward zero.
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
        return p->Coefficient / Power10(value.Type.Scale);
    }
    if (auto p = std::get_if<double>(&value.Data)) {
        if (std::isfinite(*p) && *p >= -std::ldexp(1.0, 63) && *p < std::ldexp(1.0, 64)) {
            return static_cast<__int128>(*p);
        }
        return {};
    }
    if (auto p = std::get_if<std::string>(&value.Data)) {
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
        double result;
        const auto parsed = std::from_chars(p->data(), p->data() + p->size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == p->data() + p->size() && std::isfinite(result)) {
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
        return {target, FormatValue(value)};
    }
    if (target.Kind == K::Bool) {
        if (const auto* string = std::get_if<std::string>(&value.Data)) {
            const auto lowered = Lower(*string);
            if (lowered == "true" || lowered == "false") {
                return {target, lowered == "true"};
            }
        } else if (auto number = FloatingValue(value)) {
            return {target, *number != 0};
        }
        return NullValue(target);
    }
    if (target.Kind == K::Decimal) {
        if (auto decimal = std::get_if<TDecimal>(&value.Data)) {
            auto coefficient = decimal->Coefficient;
            if (target.Scale < value.Type.Scale) {
                coefficient /= Power10(value.Type.Scale - target.Scale);
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
            if (std::isfinite(result)) {
                return {target, result};
            }
        }
        return NullValue(target);
    }
    if (target.IsIntegral() || target.Kind == K::Interval || IsDate(target.Kind)) {
        if (auto number = IntegralValue(value)) {
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
            valid = FitsSigned(*number, value.Type.Kind == K::Interval ? 64 : Width(value.Type.Kind));
        }
    } else if (value.Type.IsIntegral() || IsDate(value.Type.Kind)) {
        if (const auto* number = std::get_if<std::uint64_t>(&value.Data)) {
            const int width = value.Type.Kind == K::Date ? 16 : value.Type.Kind == K::Datetime ? 32 :
                value.Type.Kind == K::Timestamp ? 64 : Width(value.Type.Kind);
            valid = FitsUnsigned(*number, width);
        }
    } else if (value.Type.IsString()) {
        valid = std::holds_alternative<std::string>(value.Data);
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
    Fail(position, "Unsupported scalar type: " + std::string(name));
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
    if ((*this)[input].Type == target) {
        return input;
    }
    return Append({O::Convert, target, position, {input}, {}, {}, 0});
}

TExprId TExpressionArena::Cast(TExprId input, TType target, TSourcePosition position) {
    ValidateType(target, position);
    const auto source = (*this)[input].Type;
    if (target.Kind == K::Null) {
        Fail(position, "Cannot cast to Null");
    }
    if (source.Required() == target.Required()) {
        target.Nullable |= source.Nullable;
        return Convert(input, target, position);
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
        if (op == O::Negate && type.IsIntegral() && !type.IsSigned()) {
            type = CommonType(type, {K::Int8}, position);
            input = Convert(input, type, position);
        }
    } else if (op == O::Length || op == O::Lower || op == O::Upper) {
        if (!type.IsString()) {
            Fail(position, "String function expects String or Utf8");
        }
        if (op == O::Length) {
            type = {K::Uint64, type.Nullable};
        }
    } else {
        Fail(position, "Unsupported unary operation");
    }
    return Append({op, type, position, {input}, {}, {}, 0});
}

TExprId TExpressionArena::Binary(O op, TExprId left, TExprId right, TSourcePosition position) {
    const auto leftType = (*this)[left].Type;
    const auto rightType = (*this)[right].Type;
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
        arity(3);
        return If(arguments[0], arguments[1], arguments[2], position);
    }
    for (const auto& [function, op] : {std::pair{"abs", O::Abs}, {"length", O::Length}, {"lower", O::Lower}, {"upper", O::Upper},
        {"string::tolower", O::Lower}, {"string::toupper", O::Upper}}) {
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
        return Cast(arguments[0], *castType, position);
    }
    Fail(position, "Unsupported scalar function: " + std::string(name));
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
        TExprId result;
        switch (node.Op) {
            case O::Literal: result = Literal(node.Type, node.Value, node.Position); break;
            case O::Column: result = bindColumn(node.ColumnId, node.Type, node.Position); break;
            case O::Parameter: result = Parameter(node.Name, node.Type, node.Position); break;
            case O::Convert:
                result = Convert(node.Arguments[0], CommonType(node.Type, (*this)[node.Arguments[0]].Type, node.Position), node.Position);
                break;
            case O::Cast: result = Cast(node.Arguments[0], node.Type, node.Position); break;
            case O::If: result = If(node.Arguments[0], node.Arguments[1], node.Arguments[2], node.Position); break;
            case O::Coalesce: result = Coalesce(node.Arguments, node.Position); break;
            case O::Substring: result = Call("Substring", node.Arguments, node.Position); break;
            default:
                result = node.Arguments.size() == 1 ? Unary(node.Op, node.Arguments[0], node.Position) :
                    Binary(node.Op, node.Arguments[0], node.Arguments[1], node.Position);
        }
        copied.emplace(id, result);
    }
    return copied.at(root);
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
        out.precision(std::numeric_limits<double>::max_digits10);
        out << *p;
        return out.str();
    }
    const auto coefficient = std::get<TDecimal>(value.Data).Coefficient;
    auto text = Int128ToString(coefficient < 0 ? -coefficient : coefficient);
    if (value.Type.Scale) {
        if (text.size() <= value.Type.Scale) {
            text.insert(0, value.Type.Scale + 1 - text.size(), '0');
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
            const auto a = left.Type.Kind == K::Decimal ? std::get<TDecimal>(left.Data).Coefficient : *IntegralValue(left);
            const auto b = right.Type.Kind == K::Decimal ? std::get<TDecimal>(right.Data).Coefficient : *IntegralValue(right);
            order = (a > b) - (a < b);
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
            overflow = __builtin_mul_overflow(a, b, &result);
            if (decimal && !overflow) {
                result /= Power10(node.Type.Scale);
            }
            break;
        case O::Divide:
            if (decimal) {
                overflow = __builtin_mul_overflow(a, Power10(node.Type.Scale), &result);
                if (!overflow) {
                    result /= b;
                }
            } else {
                result = a / b;
            }
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
