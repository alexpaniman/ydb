PY3TEST()

TEST_SRCS(test_skip_yql.py)

DEPENDS(
    contrib/tools/protoc/bin
    ydb/tests/tools/parsed_query_bench/skip_yql
)

DATA(
    arcadia/ydb/tests/tools/parsed_query_bench/proto/logical_plan.proto
    arcadia/ydb/tests/tools/parsed_query_bench/proto/native_query.proto
)

PEERDIR(
    library/python/testing/yatest_common
    ydb/public/api/protos
)

END()
