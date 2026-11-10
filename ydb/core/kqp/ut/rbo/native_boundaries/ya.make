UNITTEST_FOR(ydb/core/kqp/opt/rbo)

FORK_SUBTESTS()

SRCS(
    kqp_native_range_policy_ut.cpp
    kqp_native_olap_policy_ut.cpp
)

PEERDIR(
    ydb/core/kqp/opt
    yql/essentials/minikql/comp_nodes
    yql/essentials/providers/common/mkql
    yql/essentials/minikql/invoke_builtins
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
