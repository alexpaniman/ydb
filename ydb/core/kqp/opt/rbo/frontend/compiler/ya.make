LIBRARY()

SRCS(compiler.cpp)

PEERDIR(
    ydb/core/kqp/opt/rbo/frontend
    ydb/core/kqp/opt/rbo/frontend/grammar
)

END()

RECURSE_FOR_TESTS(ut)
