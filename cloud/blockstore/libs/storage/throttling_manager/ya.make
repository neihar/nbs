LIBRARY()

SRCS(
    throttling_manager.cpp
)

PEERDIR(
    cloud/blockstore/config
    cloud/blockstore/libs/kikimr
    cloud/blockstore/libs/storage/api
    contrib/ydb/library/actors/core
)

END()

# TODO
# RECURSE_FOR_TESTS(
#     ut
# )
