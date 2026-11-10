#include "quoted.h"

namespace NKikimr::NKqp::NFrontend {
namespace {

unsigned Digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return 16;
}

void AppendUtf8(std::uint32_t value, std::string& output) {
    if (value < 0x80) output += static_cast<char>(value);
    else if (value < 0x800) {
        output += static_cast<char>(0xc0 | (value >> 6));
        output += static_cast<char>(0x80 | (value & 0x3f));
    } else if (value < 0x10000) {
        output += static_cast<char>(0xe0 | (value >> 12));
        output += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (value & 0x3f));
    } else {
        output += static_cast<char>(0xf0 | (value >> 18));
        output += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
        output += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (value & 0x3f));
    }
}

} // namespace

std::size_t AppendQuotedEscape(std::string_view text, std::string& output, TSourcePosition position) {
    if (text.empty()) throw TFrontendError(position, "expected string escape");
    const char escape = text.front();
    unsigned base = 16, digits = 0;
    std::uint32_t value = 0;
    switch (escape) {
        case 'a': output += '\a'; return 1;
        case 'b': output += '\b'; return 1;
        case 'f': output += '\f'; return 1;
        case 'n': output += '\n'; return 1;
        case 'r': output += '\r'; return 1;
        case 't': output += '\t'; return 1;
        case 'v': output += '\v'; return 1;
        case '0': case '1': case '2': case '3': base = 8; digits = 2; value = escape - '0'; break;
        case 'x': digits = 2; break;
        case 'u': digits = 4; break;
        case 'U': digits = 8; break;
        // SQL accepts other escaped characters literally, including quotes,
        // backslashes and digits outside the three-digit octal form.
        default: output += escape; return 1;
    }
    if (text.size() <= digits) throw TFrontendError(position, "incomplete numeric string escape");
    for (unsigned i = 1; i <= digits; ++i) {
        const auto digit = Digit(text[i]);
        if (digit >= base) throw TFrontendError(position, "invalid numeric string escape");
        value = value * base + digit;
    }
    if (escape == 'u' || escape == 'U') {
        if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            throw TFrontendError(position, "invalid Unicode string escape");
        AppendUtf8(value, output);
    } else output += static_cast<char>(value);
    return digits + 1;
}

} // namespace NKikimr::NKqp::NFrontend
