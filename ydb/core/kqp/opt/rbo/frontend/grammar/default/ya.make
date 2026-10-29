LIBRARY()

# The grammar is data at build time. No YQL parser or translation library is linked.
SET(SQL_GRAMMAR ${BINDIR}/SQLv1Antlr4.g4)
SET(GRAMMAR_STRING_CORE_SINGLE "\"~(['#BACKSLASH#]) | (BACKSLASH .)\"")
SET(GRAMMAR_STRING_CORE_DOUBLE "\"~([#DOUBLE_QUOTE##BACKSLASH#]) | (BACKSLASH .)\"")
SET(GRAMMAR_MULTILINE_COMMENT_CORE "\".\"")
CONFIGURE_FILE(${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in ${SQL_GRAMMAR})
RUN_ANTLR4_CPP(${SQL_GRAMMAR} -package NativeRboDefault)

END()
