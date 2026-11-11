// Original workload structure checks from PR #58587. Native schemas and scalar
// IDs replace annotated YQL nodes; SQL resources and relational expectations stay intact.
#include <ydb/core/kqp/opt/rbo/kqp_native_frontend.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_test_helpers.h>
#include <ydb/library/testlib/helpers.h>

#include <library/cpp/resource/resource.h>

#include <regex>
#include <sstream>

namespace NKikimr::NKqp {
namespace {

NFrontend::TCatalogTable ClickBenchTable() {
    NFrontend::TCatalogTable table{"/Root/hits", {}};
    const auto schema = NResource::Find("clickbench/schema.sql");
    std::istringstream lines(std::string(schema.data(), schema.size()));
    const std::regex column(R"(^\s*(\w+)\s+(\w+)\s+(NOT NULL)\s*,\s*$)");
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("CREATE TABLE") != std::string::npos || line.find("PRIMARY KEY") != std::string::npos
            || line.find(");") != std::string::npos || line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::smatch match;
        UNIT_ASSERT_C(std::regex_match(line, match, column), "Unsupported schema line: " << line);
        const auto type = match[2].str();
        table.Columns.push_back({match[1].str(), NFrontend::ParseType(type == "Bytes" ? "String" : type)});
    }
    UNIT_ASSERT(!table.Columns.empty());
    return table;
}

void CheckClickBench(ui32 queryId, bool columnStore, const TVector<TString>& outputs,
    const TVector<TString>& sortKeys, TStringBuf predicateColumn) {
    NTests::TIdTestContext f;
    const auto sql = NResource::Find(TStringBuilder() << "clickbench/q" << queryId << ".yql");
    const auto metadata = ClickBenchTable();
    const auto storage = columnStore ? NYql::EStorageType::ColumnStorage : NYql::EStorageType::RowStorage;
    const auto nodes = f.ExprCtx.NodeAllocationCounter;
    const auto types = f.ExprCtx.TypeNodes.size();
    TIntrusivePtr<TOpRoot> root;
    {
        auto query = CompileNativeToRbo(std::string_view(sql.data(), sql.size()), [&](std::string_view path, auto) {
            UNIT_ASSERT_VALUES_EQUAL(path, metadata.Name);
            return TNativeTable{metadata, TRboTable{"/Root/hits", "1:1", "", "1"}, storage};
        }, f.ExprCtx);
        UNIT_ASSERT_VALUES_EQUAL(query.Roots.size(), 1);
        root = query.Roots.front();
    }
    // Check ownership after the importer and its handoff have been destroyed.
    NTests::AssertIdInvariants(*root, root->PlanProps);
    UNIT_ASSERT(root->ComputeTypes(f.RboCtx) == IGraphTransformer::TStatus::Ok);
    UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.NodeAllocationCounter, nodes);
    UNIT_ASSERT_VALUES_EQUAL(f.ExprCtx.TypeNodes.size(), types);
    TVector<TString> actualOutputs;
    for (const auto& [id, name] : root->GetColumns().Items()) {
        Y_UNUSED(id);
        actualOutputs.push_back(name);
    }
    UNIT_ASSERT_VALUES_EQUAL(actualOutputs, outputs);
    auto op = root->GetInput();
    if (!sortKeys.empty()) {
        UNIT_ASSERT(op->Kind == EOperator::Limit);
        const auto limit = CastOperator<TOpLimit>(op);
        UNIT_ASSERT(!limit->HasOffset());
        const auto& count = limit->GetLimitCond();
        const auto& value = (*count.GetNativeArena())[count.GetNativeRoot()];
        UNIT_ASSERT(value.Op == NFrontend::EOpcode::Literal);
        UNIT_ASSERT(value.Type == (NFrontend::TType{NFrontend::ETypeKind::Uint64}));
        UNIT_ASSERT_VALUES_EQUAL(std::get<std::uint64_t>(value.Value), 10);
        op = limit->GetInput();
        UNIT_ASSERT(op->Kind == EOperator::Sort);
        const auto sort = CastOperator<TOpSort>(op);
        TVector<TString> actualKeys;
        for (const auto& [id, order] : sort->GetSortElements().Items()) {
            actualKeys.push_back(root->PlanProps.InfoUnitRegistry.Get(id).GetColumnName());
            UNIT_ASSERT(order.Ascending);
        }
        UNIT_ASSERT_VALUES_EQUAL(actualKeys, sortKeys);
        op = sort->GetInput();
    }
    // Native identity projections reuse source IUs instead of adding the
    // reference importer's identity Map. Check those bindings below.
    UNIT_ASSERT(op->Kind == EOperator::Filter);
    const auto filter = CastOperator<TOpFilter>(op);
    const auto inputs = filter->GetFilterExpression().GetRawInputIUs();
    UNIT_ASSERT_VALUES_EQUAL(inputs.Size(), 1);
    UNIT_ASSERT_VALUES_EQUAL(root->PlanProps.InfoUnitRegistry.Get(*inputs.begin()).GetColumnName(), predicateColumn);
    op = filter->GetInput();
    UNIT_ASSERT(op->Kind == EOperator::Source);
    const auto read = CastOperator<TOpRead>(op);
    UNIT_ASSERT(read->GetTableStorageType() == storage);
    UNIT_ASSERT_VALUES_EQUAL(read->TableRef.Path, "/Root/hits");
    // Preserve the full catalog-to-IU contract; pruning belongs to RBO rules.
    UNIT_ASSERT_VALUES_EQUAL(read->GetColumns().Size(), metadata.Columns.size());
    for (const auto& [id, name] : root->GetColumns().Items()) {
        UNIT_ASSERT(read->GetColumns().Contains(id));
        UNIT_ASSERT_VALUES_EQUAL(root->PlanProps.InfoUnitRegistry.Get(id).GetColumnName(), name);
    }
    for (const auto id : read->GetColumns()) {
        const auto& name = root->PlanProps.InfoUnitRegistry.Get(id).GetColumnName();
        UNIT_ASSERT(read->GetNativeIUType(id) == read->TableRef.NativeColumns->at(name));
    }
    for (const auto& item : *root) {
        for (const auto& expression : item.Current->GetExpressions()) {
            UNIT_ASSERT(expression.get().IsNative());
            UNIT_ASSERT_EQUAL(expression.get().PlanProps, &root->PlanProps);
        }
    }
}
} // namespace

Y_UNIT_TEST_SUITE(KqpNativeWorkloadLowering) {
    Y_UNIT_TEST_TWIN(ClickBench_Q20, ColumnStore) {
        CheckClickBench(20, ColumnStore, {"UserID"}, {}, "UserID");
    }
    Y_UNIT_TEST_TWIN(ClickBench_Q25, ColumnStore) {
        CheckClickBench(25, ColumnStore, {"SearchPhrase", "EventTime"}, {"EventTime"}, "SearchPhrase");
    }
    Y_UNIT_TEST_TWIN(ClickBench_Q26, ColumnStore) {
        CheckClickBench(26, ColumnStore, {"SearchPhrase"}, {"SearchPhrase"}, "SearchPhrase");
    }
    Y_UNIT_TEST_TWIN(ClickBench_Q27, ColumnStore) {
        CheckClickBench(27, ColumnStore, {"SearchPhrase", "EventTime"}, {"EventTime", "SearchPhrase"}, "SearchPhrase");
    }
}
} // namespace NKikimr::NKqp
