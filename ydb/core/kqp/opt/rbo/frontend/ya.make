LIBRARY()

SRCS(
    binder.cpp
    expression.cpp
    parser.cpp
)

END()

RECURSE_FOR_TESTS(ut)
