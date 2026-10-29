#pragma once

#include "../binder.h"
#include "../diagnostic.h"

namespace NKikimr::NKqp::NFrontend {

enum class ECompilerMode { NativeRequired, Auto, Legacy };
enum class EParser { Grammar, Handwritten, Verify };

struct TCompilerSettings {
    ECompilerMode Mode = ECompilerMode::NativeRequired;
    EParser Parser = EParser::Grammar;
    TParseSettings Parsing;
    TBindSettings Binding;
};

struct TUnsupported {
    EFeature Feature;
    TSourcePosition Position;
    std::string Message;
};

struct TCompileResult {
    std::optional<TBoundProgram> Program;
    std::optional<TUnsupported> Unsupported;
    bool UsesLegacy() const { return !Program; }
};

struct TTableRequest {
    std::string Name;
    TSourcePosition Position;
};

// Parsing and binding can be separated by asynchronous catalog I/O. Both use
// only native types. The generated parser is the default; Verify compares the
// complete supported syntax structure before permitting handwritten output.
TParsedQuery ParseNative(std::string_view sql, const TCompilerSettings& settings = {});
std::vector<TTableRequest> CollectTables(const TParsedQuery& syntax, const TBindSettings& settings = {});
TBoundProgram BindNative(const TParsedQuery& syntax, const TTableResolver& catalog,
    const TBindSettings& settings = {});
TCompileResult CompileNative(std::string_view sql, const TTableResolver& catalog,
    const TCompilerSettings& settings = {});

// This is the execution profile, not a promise of support for every construct
// recognized by the full grammar. New support requires semantic parity tests.
std::string_view SupportManifest();

} // namespace NKikimr::NKqp::NFrontend
