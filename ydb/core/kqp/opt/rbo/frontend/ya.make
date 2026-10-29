LIBRARY()

RUN_PYTHON3(
    tools/grammar_keywords.py
    ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    IN ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    STDOUT grammar_keywords.inc
)

SRCS(
    binder.cpp
    expression.cpp
    parser.cpp
)

END()

RECURSE_FOR_TESTS(
    ut
    grammar/ut
    compiler/ut
)
