LIBRARY()

SRCS(grammar.cpp)

PEERDIR(
    ydb/core/kqp/opt/rbo/frontend
    ydb/core/kqp/opt/rbo/frontend/grammar/default
    ydb/core/kqp/opt/rbo/frontend/grammar/ansi
    contrib/libs/antlr4_cpp_runtime
)

END()

RECURSE_FOR_TESTS(ut)
