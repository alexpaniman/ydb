#pragma once

#include "frontend.h"

namespace NKikimr::NKqp::NFrontend {

struct TExecutionRoot {
    TQueryResult Query;
    std::uint64_t Phase = 0;
    // Internal row-list input supplied by this producer's execution result.
    // It is not an externally declared parameter or a public result set.
    std::optional<std::string> MaterializedInput;
};

struct TExecutionPlan {
    TBoundProgram Program;
    std::vector<TExecutionRoot> Roots;
};

// Resolve statement-level sharing before operator translation. Each root still
// consists solely of the ordinary native relational and scalar operations.
TExecutionPlan PrepareExecution(TCompiledProgram compiled);

} // namespace NKikimr::NKqp::NFrontend
