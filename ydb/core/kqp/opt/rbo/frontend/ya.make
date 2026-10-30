LIBRARY()

RUN_PYTHON3(
    tools/generate.py
    ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in keywords
    IN ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    STDOUT grammar_keywords.inc
)

SRCS(
    frontend.cpp
    expression.cpp
    parser.cpp
    grammar.cpp
)

PEERDIR(
    ydb/core/kqp/opt/rbo/frontend/grammar
    contrib/libs/antlr4_cpp_runtime
)

END()

RECURSE_FOR_TESTS(ut)
