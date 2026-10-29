UNITTEST()

SRCS(grammar_ut.cpp)

PEERDIR(
    ydb/core/kqp/opt/rbo/frontend/grammar
    library/cpp/resource
)

RESOURCE(
    ydb/core/kqp/ut/rbo/data/yql-tpch/q1.yql tpch-q1
    ydb/core/kqp/ut/rbo/data/yql-tpcds/q47.yql tpcds-q47
)

END()
