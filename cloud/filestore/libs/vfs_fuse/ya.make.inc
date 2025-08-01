SRCS(
    config.cpp
    directory_handles_storage.cpp
    fs.cpp
    fs_directory_handle.cpp
    fs_impl.cpp
    fs_impl_attr.cpp
    fs_impl_data.cpp
    fs_impl_list.cpp
    fs_impl_lock.cpp
    fs_impl_node.cpp
    fs_impl_stat.cpp
    handle_ops_queue.cpp
    log.cpp
    loop.cpp
    node_cache.cpp
)

PEERDIR(
    cloud/filestore/config
    cloud/filestore/libs/client
    cloud/filestore/libs/diagnostics
    cloud/filestore/libs/service
    cloud/filestore/libs/vfs
    cloud/filestore/libs/vfs/protos
    cloud/filestore/libs/vfs_fuse/protos
    cloud/filestore/libs/vfs_fuse/write_back_cache

    cloud/storage/core/libs/common
    cloud/storage/core/libs/diagnostics

    library/cpp/lwtrace
    library/cpp/threading/future
    library/cpp/threading/future/subscription
)
