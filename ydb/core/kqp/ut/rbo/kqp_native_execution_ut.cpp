#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

namespace NKikimr::NKqp {
namespace {
using namespace NYdb;
using namespace NYdb::NTable;

TKikimrSettings Settings(bool native) {
    NKikimrConfig::TAppConfig config;
    auto* service = config.MutableTableServiceConfig();
    service->SetEnableNewRBO(native);
    service->SetNativeFrontendMode(native ? NKikimrConfig::TTableServiceConfig::NATIVE_REQUIRED
                                        : NKikimrConfig::TTableServiceConfig::NATIVE_LEGACY);
    service->SetEnableFallbackToYqlOptimizer(false);
    service->SetEnableFallbackOnMultipleStatements(false);
    return TKikimrSettings(config).SetWithSampleTables(false);
}

template <class TResult>
void Success(const TResult& result) {
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    UNIT_ASSERT_C(result.GetIssues().ToString().find("[native_frontend_fallback]") == std::string::npos,
        result.GetIssues().ToString());
}

void CreateTables(TKikimrRunner& kikimr, TSession& session) {
    const auto schema = session.ExecuteSchemeQuery(R"(
        CREATE TABLE t (id Uint64 NOT NULL, PRIMARY KEY(id));
        CREATE TABLE u (id Uint64 NOT NULL, value String, PRIMARY KEY(id));
    )").GetValueSync();
    UNIT_ASSERT_C(schema.IsSuccess(), schema.GetIssues().ToString());
    TValueBuilder rows;
    rows.BeginList();
    for (ui64 id : {1ul, 2ul, 3ul}) rows.AddListItem().BeginStruct().AddMember("id").Uint64(id).EndStruct();
    rows.EndList();
    const auto inserted = kikimr.GetTableClient().BulkUpsert("/Root/t", rows.Build()).GetValueSync();
    UNIT_ASSERT_C(inserted.IsSuccess(), inserted.GetIssues().ToString());
}
}

Y_UNIT_TEST_SUITE(KqpNativeExecutionRuntime) {
    // Exact shared-binding cases from the newer reference frontend. Native
    // execution must preserve sharing without its legacy fallback.
    Y_UNIT_TEST_TWIN(SharedVolatileAliasesPreserveOneEvaluation, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto queries = session.ExecuteDataQuery(R"(
            $q = SELECT id, CAST(RandomUuid(id) AS Utf8) AS value FROM t;
            $alias = $q;
            SELECT * FROM $q ORDER BY id;
            SELECT * FROM $alias ORDER BY id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(queries);
        UNIT_ASSERT_VALUES_EQUAL(queries.GetResultSets().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(queries.GetResultSet(0).RowsCount(), 3);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(queries.GetResultSet(0)), FormatResultSetYson(queries.GetResultSet(1)));

        const auto scalar = session.ExecuteDataQuery(R"(
            $x = RandomUuid(1);
            SELECT CAST($x AS Utf8) AS a, CAST($x AS Utf8) AS b;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(scalar);
        UNIT_ASSERT_VALUES_EQUAL(scalar.GetResultSets().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(scalar.GetResultSet(0).RowsCount(), 1);
        TResultSetParser row(scalar.GetResultSet(0));
        UNIT_ASSERT(row.TryNextRow());
        UNIT_ASSERT_VALUES_EQUAL(row.ColumnParser(0).GetUtf8(), row.ColumnParser(1).GetUtf8());
    }

    Y_UNIT_TEST_TWIN(NamedVolatileRowsAreSharedAcrossResultsAndWrites, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = session.ExecuteDataQuery(R"(
            $q = SELECT id, CAST(RandomUuid(id) AS String) AS value FROM t;
            SELECT id, value FROM $q ORDER BY id;
            INSERT INTO u SELECT id, value FROM $q;
            SELECT id, value FROM $q ORDER BY id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSet(0).RowsCount(), 3);
        const auto expected = FormatResultSetYson(result.GetResultSet(0));
        UNIT_ASSERT_VALUES_EQUAL(expected, FormatResultSetYson(result.GetResultSet(1)));
        const auto written = session.ExecuteDataQuery("SELECT id, Unwrap(value) AS value FROM u ORDER BY id",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(written);
        // The classic frontend expands the write independently. Native named
        // producers retain their identity across public execution roots.
        if constexpr (Native) UNIT_ASSERT_VALUES_EQUAL(expected, FormatResultSetYson(written.GetResultSet(0)));
        else UNIT_ASSERT_VALUES_UNEQUAL(expected, FormatResultSetYson(written.GetResultSet(0)));
    }

    Y_UNIT_TEST_TWIN(NamedVolatileRowsAreSharedWithinOneResult, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = session.ExecuteDataQuery(R"(
            $q = SELECT id, RandomUuid(id) AS value FROM t;
            SELECT l.value = r.value AS equal FROM $q AS l JOIN $q AS r ON l.id = r.id ORDER BY l.id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        // Pre-migration new RBO also shares this table-backed projection; the
        // classic optimizer evaluates the two join inputs independently.
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)),
            Native ? "[[%true];[%true];[%true]]" : "[[%false];[%false];[%false]]");
    }

    Y_UNIT_TEST_TWIN(GlobalVolatileBindingsHaveOneValueAcrossStatements, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto result = session.ExecuteDataQuery(R"(
            $value = YQL::RandomUuid(); $copy = $value;
            SELECT CAST($value AS String) AS value;
            SELECT CAST($copy AS String) AS value;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), FormatResultSetYson(result.GetResultSet(1)));
    }

    Y_UNIT_TEST_TWIN(ZeroArgumentUuidIsOneInternalInputPerExecution, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto prepared = session.PrepareDataQuery("SELECT CAST(YQL::RandomUuid() AS String) AS a, CAST(YQL::RandomUuid() AS String) AS b").GetValueSync();
        UNIT_ASSERT_C(prepared.IsSuccess(), prepared.GetIssues().ToString());
        UNIT_ASSERT(prepared.GetQuery().GetParameterTypes().empty());
        std::optional<TString> previous;
        for (unsigned i = 0; i < 2; ++i) {
            const auto result = prepared.GetQuery().Execute(TTxControl::BeginTx().CommitTx()).GetValueSync();
            Success(result);
            TResultSetParser rows(result.GetResultSet(0));
            UNIT_ASSERT(rows.TryNextRow());
            const TString value(rows.ColumnParser(0).GetString());
            UNIT_ASSERT_VALUES_EQUAL(value, rows.ColumnParser(1).GetString());
            if (previous) UNIT_ASSERT_VALUES_UNEQUAL(value, *previous);
            previous = value;
        }
    }

    Y_UNIT_TEST_TWIN(ZeroArgumentNumberIsOneInternalInputPerExecution, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto prepared = session.PrepareDataQuery(R"(
            SELECT YQL::RandomNumber() AS a, YQL::RandomNumber() AS b;
            SELECT YQL::RandomNumber() AS c;
        )").GetValueSync();
        UNIT_ASSERT_C(prepared.IsSuccess(), prepared.GetIssues().ToString());
        UNIT_ASSERT(prepared.GetQuery().GetParameterTypes().empty());
        std::optional<ui64> previous;
        for (unsigned i = 0; i < 2; ++i) {
            const auto result = prepared.GetQuery().Execute(TTxControl::BeginTx().CommitTx()).GetValueSync();
            Success(result);
            UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
            TResultSetParser first(result.GetResultSet(0)), second(result.GetResultSet(1));
            UNIT_ASSERT(first.TryNextRow()); UNIT_ASSERT(second.TryNextRow());
            const auto value = first.ColumnParser(0).GetUint64();
            UNIT_ASSERT_VALUES_EQUAL(value, first.ColumnParser(1).GetUint64());
            UNIT_ASSERT_VALUES_EQUAL(value, second.ColumnParser(0).GetUint64());
            if (previous) UNIT_ASSERT_VALUES_UNEQUAL(value, *previous);
            previous = value;
        }
    }

    // Retain the established generic-query behavior tested by
    // NamedExpressionPureNondeterministicWriteAndResult in the legacy suite.
    Y_UNIT_TEST_TWIN(DependencyBearingScalarIsNotQueryCached, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        CreateTables(kikimr, session);
        const auto result = kikimr.GetQueryClient().ExecuteQuery(R"(
            $value = CAST(RandomUuid(1u) AS String);
            UPSERT INTO u (id, value) VALUES (1ul, $value);
            SELECT $value AS value;
        )", NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(result);
        const auto stored = session.ExecuteDataQuery("SELECT value FROM u WHERE id = 1ul",
            TTxControl::BeginTx().CommitTx()).GetValueSync();
        Success(stored);
        TResultSetParser selected(result.GetResultSet(0)), written(stored.GetResultSet(0));
        UNIT_ASSERT(selected.TryNextRow()); UNIT_ASSERT(written.TryNextRow());
        UNIT_ASSERT_VALUES_UNEQUAL(selected.ColumnParser(0).GetString(), *written.ColumnParser(0).GetOptionalString());
    }

    Y_UNIT_TEST_TWIN(UnchosenGlobalAssertionStaysLazy, Native) {
        TKikimrRunner kikimr(Settings(Native));
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        const auto params = TParamsBuilder().AddParam("$chosen").Bool(false).Build().Build();
        const auto result = session.ExecuteDataQuery(R"(
            DECLARE $chosen AS Bool;
            $value = Ensure(YQL::RandomUuid(), false, 'unchosen global assertion');
            SELECT IF($chosen, CAST($value AS String), 'safe') AS value;
            SELECT IF($chosen, CAST($value AS String), 'safe') AS value;
        )", TTxControl::BeginTx().CommitTx(), params).GetValueSync();
        Success(result);
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 2);
        for (const auto& rows : result.GetResultSets()) UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(rows), "[[\"safe\"]]");
    }
}
}
