PY3TEST()

TEST_SRCS(
    test_compile_trace.py
)

PEERDIR(
    ydb/core/kqp/tracing/tools/compile_trace
)

END()
