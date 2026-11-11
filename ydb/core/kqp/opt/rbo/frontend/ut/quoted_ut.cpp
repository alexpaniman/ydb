#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/quoted.h>
#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

Y_UNIT_TEST_SUITE(KqpNativeQuotedText) {
    Y_UNIT_TEST(DiagnosticLiteralsRoundTripArbitraryBytesThroughBothParsers) {
        std::vector<std::string> values{"", "plain text", "quotes\"'\\", "\n\r\t\a\b\f\v", "é😀"};
        auto& bytes = values.emplace_back();
        for (unsigned byte = 0; byte < 256; ++byte) {
            bytes += static_cast<char>(byte);
            bytes += "0123456789ABCDEF";
        }
        for (const auto& value : values) {
            const auto quoted = QuoteStringLiteral(value);
            UNIT_ASSERT(quoted.front() == '"' && quoted.back() == '"');
            for (const unsigned char byte : quoted) UNIT_ASSERT(byte >= 0x20 && byte < 0x7f);
            for (const auto parser : {EParser::Handwritten, EParser::Grammar}) {
                TCompilerSettings settings; settings.Parser = parser;
                const auto compiled = CompileNative("SELECT " + quoted, {}, settings);
                const auto& program = compiled.Program->Get();
                const auto output = program.Results.front().Columns.front().Column;
                bool checked = false;
                for (const auto& relation : program.Relations) for (const auto& definition : relation.Definitions)
                    if (definition.Column == output) {
                        const auto result = Evaluate(program.Expressions, definition.Expression);
                        UNIT_ASSERT(result.Type == TType{ETypeKind::String});
                        UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(result.Data), value);
                        checked = true;
                    }
                UNIT_ASSERT(checked);
            }
        }
    }

    Y_UNIT_TEST(DiagnosticLiteralsHaveOnePairOfQuotes) {
        UNIT_ASSERT_VALUES_EQUAL(QuoteStringLiteral("text"), "\"text\"");
        UNIT_ASSERT_VALUES_EQUAL(QuoteStringLiteral("\"\\\n\r\t"), "\"\\\"\\\\\\n\\r\\t\"");
        UNIT_ASSERT_VALUES_EQUAL(QuoteStringLiteral(std::string("\0\xff", 2)), "\"\\x00\\xFF\"");
    }
}
