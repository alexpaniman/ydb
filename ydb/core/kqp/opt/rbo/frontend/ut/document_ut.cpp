#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>
#include <ydb/core/kqp/opt/rbo/frontend/quoted.h>
#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NKqp::NFrontend;

namespace {
using K = ETypeKind;
using O = EOpcode;
TCompiledProgram Compile(std::string_view sql, EParser parser = EParser::Fast) {
    TCompilerSettings settings;
    settings.Parser = parser;
    settings.Mode = ECompilerMode::Auto;
    auto result = CompileNative(sql, [](const TTableAddress&, TSourcePosition) -> TCatalogTable {
        throw TFrontendError({}, "literal compilation must not request a catalog");
    }, settings);
    UNIT_ASSERT_C(result.Program, sql);
    return std::move(*result.Program);
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeDocuments) {
    // Complete literal-type case from PR #58587, including both document suffixes.
    Y_UNIT_TEST(LiteralsUseExistingTypeAnnotation) {
        for (const auto parser : {EParser::Fast, EParser::Grammar}) {
            for (const auto& [sql, type] : std::vector<std::pair<std::string, std::string>>{
                    {"42", "Int32"}, {"2147483648", "Int64"}, {"9223372036854775808", "Uint64"},
                    {"18446744073709551615ul", "Uint64"}, {"0x2a", "Int32"}, {"0o52", "Int32"}, {"0b101010", "Int32"},
                    {"1u", "Uint32"}, {"1l", "Int64"}, {"127t", "Int8"}, {"255ut", "Uint8"},
                    {"32767s", "Int16"}, {"65535us", "Uint16"}, {"1.25", "Double"}, {"1.25f", "Float"},
                    {"TRUE", "Bool"}, {"NULL", "Null"}, {"'text'", "String"}, {"'text'u", "Utf8"},
                    {"'{}'j", "Json"}, {"'{}'y", "Yson"}}) {
                const auto program = Compile("SELECT " + sql, parser);
                UNIT_ASSERT_VALUES_EQUAL_C(program.Get().Results.at(0).Columns.at(0).Type.ToString(), type, sql);
            }
            const auto program = Compile("PRAGMA UnicodeLiterals; SELECT '{}'J AS j, '{}'Y AS y, Json('{}') AS jc, Yson('{}') AS yc", parser);
            const auto& columns = program.Get().Results.at(0).Columns;
            for (unsigned i = 0; i < columns.size(); ++i) UNIT_ASSERT(columns[i].Type.Kind == (i % 2 ? K::Yson : K::Json));
        }
    }

    Y_UNIT_TEST(ValidateDocumentBytesAndKeepSourcePositions) {
        const TSourcePosition position{31, 4, 8};
        for (const auto kind : {K::Json, K::Yson}) {
            TExpressionArena arena;
            const std::vector<std::string> valid = kind == K::Json
                ? std::vector<std::string>{"{}", "[]", "null", "true", "-12.5e3", "18446744073709551615", R"({"k":[1,"value",null]})", R"("\u00e9")"}
                : std::vector<std::string>{"{}", "[]", "#", "%true", "%nan", "%inf", "18446744073709551615u", "[1;2u;#;]", R"(<a=1;>{k="v";})", std::string("\x01\x06" "abc", 5)};
            for (const auto& text : valid) {
                const auto id = arena.Literal({kind}, text, position);
                UNIT_ASSERT_VALUES_EQUAL(std::get<std::string>(Evaluate(arena, id).Data), text);
                for (const auto parser : {EParser::Fast, EParser::Grammar})
                    Compile("SELECT " + QuoteStringLiteral(text) + (kind == K::Json ? "j" : "y"), parser);
            }
            const std::vector<std::string> invalid = kind == K::Json
                ? std::vector<std::string>{"", "[123}", "[1],[2]", R"({"c",})", "{'one':1}", "1e400", "%nan", std::string("\"\xff\"", 3)}
                : std::vector<std::string>{"", "[123}", "[1];[2]", "#;#", "1;2", R"({"c";})", "{'one'=1}", "<a=1>", std::string("\x01\x08" "abc", 5)};
            for (const auto& text : invalid) {
                try { arena.Literal({kind}, text, position); UNIT_FAIL("invalid document accepted"); }
                catch (const TUnsupportedFeature&) { UNIT_FAIL("malformed literal must be an error, not fallback"); }
                catch (const TFrontendError& error) {
                    UNIT_ASSERT_VALUES_EQUAL(error.Position.Offset, position.Offset);
                    UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, position.Line);
                    UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, position.Column);
                }
                for (const auto parser : {EParser::Fast, EParser::Grammar}) {
                    try { Compile("SELECT\n  " + QuoteStringLiteral(text) + (kind == K::Json ? "j" : "y"), parser); UNIT_FAIL("invalid SQL literal accepted"); }
                    catch (const TUnsupportedFeature&) { UNIT_FAIL("malformed SQL literal must not select fallback"); }
                    catch (const TFrontendError& error) {
                        UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, 2);
                        UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, 3);
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(FoldingAndCopiesKeepTypedDocumentBytes) {
        for (const auto kind : {K::Json, K::Yson}) {
            TExpressionArena arena;
            const auto value = arena.Literal({kind}, std::string("{}"));
            const auto conditional = arena.If(arena.Literal({K::Bool}, true), value, arena.Literal({kind, 1}, {}));
            const auto nested = arena.Unary(O::OptionalWrap, arena.Unary(O::OptionalWrap, value));
            const TExprId roots[]{conditional, nested};
            const auto folded = arena.FoldConstants(roots);
            TExpressionArena copied;
            for (const auto root : folded) {
                const auto imported = copied.Import(arena, root, {});
                UNIT_ASSERT(copied[imported].Type == arena[root].Type);
                UNIT_ASSERT_VALUES_EQUAL(FormatValue(Evaluate(copied, imported)), FormatValue(Evaluate(arena, root)));
            }
            const std::string deep = std::string(1024, '[') + "0" + std::string(1024, ']');
            UNIT_ASSERT(arena[arena.Literal({kind}, deep)].Type.Kind == kind);
            UNIT_ASSERT_EXCEPTION(arena.Binary(O::Equal, value, value), TUnsupportedFeature);
            UNIT_ASSERT_EXCEPTION(arena.Unary(O::StablePickle, value), TUnsupportedFeature);
            if (kind == K::Yson) UNIT_ASSERT_EXCEPTION(arena.JsonValue(value, "$.k"), TFrontendError);
        }
    }
}
