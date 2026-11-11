#include "document.h"

#include <library/cpp/json/json_reader.h>
#include <library/cpp/yson_pull/exceptions.h>
#include <library/cpp/yson_pull/reader.h>
#include <cmath>

namespace NKikimr::NKqp::NFrontend {
namespace {
class TJsonLiteralCallbacks final : public NJson::TJsonCallbacks {
public:
    TJsonLiteralCallbacks() : TJsonCallbacks(false) {}
    bool OnDouble(double value) override { return !std::isinf(value); }
};
} // namespace

void ValidateDocumentLiteral(ETypeKind kind, std::string_view text, TSourcePosition position) {
    const TStringBuf bytes(text.data(), text.size());
    if (kind == ETypeKind::Json) {
        NJson::TJsonReaderConfig config;
        config.UseIterativeParser = true;
        TMemoryInput input(bytes);
        TJsonLiteralCallbacks callbacks;
        if (!NJson::ReadJson(&input, &config, &callbacks)) throw TFrontendError(position, "Invalid Json literal");
        return;
    }
    if (kind == ETypeKind::Yson) {
        try {
            using E = NYsonPull::EEventType;
            NYsonPull::TReader reader(NYsonPull::NInput::FromMemory(bytes), NYsonPull::EStreamType::Node);
            if (reader.NextEvent().Type() != E::BeginStream) throw TFrontendError(position, "Invalid Yson literal");
            // Consume exactly one value, including its optional attributes.
            // Draining the reader to EndStream also accepts scalar fragments.
            // The reader validates nesting; counting it here avoids recursion.
            std::size_t depth = 0;
            for (;;) {
                const auto event = reader.NextEvent().Type();
                if (event == E::EndStream) throw TFrontendError(position, "Invalid Yson literal");
                if (event == E::BeginList || event == E::BeginMap || event == E::BeginAttributes) ++depth;
                else if (event == E::EndList || event == E::EndMap || event == E::EndAttributes) --depth;
                if (depth == 0 && event != E::EndAttributes) break;
            }
            if (reader.NextEvent().Type() != E::EndStream) throw TFrontendError(position, "Invalid Yson literal");
        } catch (const NYsonPull::NException::TBadStream&) {
            throw TFrontendError(position, "Invalid Yson literal");
        }
        return;
    }
    throw TUnsupportedFeature(EFeature::Expression, position, "JsonDocument literals require binary document encoding");
}

} // namespace NKikimr::NKqp::NFrontend
