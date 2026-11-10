LIBRARY()

# Cache grammar prediction across compilation worker threads. The generated
# runtime synchronizes shared DFA updates; keep this setting local to our parsers.
CXXFLAGS(-UANTLR4_USE_THREAD_LOCAL_CACHE -DANTLR4_USE_THREAD_LOCAL_CACHE=0)

# Independent generated parsers: the original YQL grammar is build-time data.
RUN_PYTHON3(
    ../tools/generate.py
    ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in default
    IN ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    STDOUT NativeRboDefault.g4
)
RUN_ANTLR4_CPP(${BINDIR}/NativeRboDefault.g4 -package NativeRboDefault)

RUN_PYTHON3(
    ../tools/generate.py
    ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in ansi
    IN ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    STDOUT NativeRboAnsi.g4
)
RUN_ANTLR4_CPP(${BINDIR}/NativeRboAnsi.g4 -package NativeRboAnsi)

END()
