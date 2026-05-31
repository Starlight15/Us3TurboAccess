add_library(us3_turbo_access_client
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  ${US3_TURBO_ACCESS_RDMA_PROTO_SRCS}
  src/core/async/client_executor.cpp
  src/core/client/client.cpp
  src/core/client/client_core.cpp
  src/core/client/capability_probe.cpp
  src/core/common/types.cpp
  src/core/common/errors.cpp
  src/core/common/brpc_channel.cpp
  src/core/common/channel_registry.cpp
  src/core/contracts/request_builder.cpp
  src/core/routing/transfer_path.cpp
  src/core/routing/transfer_router.cpp
  src/core/metrics/client_metrics.cpp
  src/core/upload/upload_coordinator.cpp
  src/core/gds/gds_transfer_path.cpp
  src/core/rdma/rdma_transfer_path.cpp
  src/core/http/http_transfer_path.cpp
  src/control/metadata_client.cpp
  src/data/gds_data_client.cpp
  src/data/rdma_data_plane_client.cpp
  src/data/http_data_client.cpp
  src/data/http_crc32c.cpp
  src/transports/gds/gds_memory_registry.cpp
  src/transports/gds/cuobj_library.cpp
  src/transports/gds/chunk_dispatcher.cpp
  src/transports/gds/cuobject_client.cpp
  src/transports/rdma/host_memory_registry.cpp
  src/transports/rdma/pinned_buffer.cpp
  src/transports/rdma/rdma_cm_connection.cpp
  src/transports/rdma/rdma_completion_driver.cpp
  src/transports/rdma/rdma_connection_pool.cpp
  src/transports/rdma/rdma_resources.cpp
  src/transports/rdma/rdma_transfer_engine.cpp
  src/transports/http/http_channel.cpp)
add_dependencies(us3_turbo_access_client
  us3_turbo_access_control_plane_proto
  us3_turbo_access_rdma_data_plane_proto)

add_library(Us3TurboAccess::client ALIAS us3_turbo_access_client)

set_target_properties(us3_turbo_access_client PROPERTIES OUTPUT_NAME us3_turbo_access_client)

target_include_directories(us3_turbo_access_client
  PUBLIC
    ${PROJECT_SOURCE_DIR}/client/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/client/src
    ${US3_TURBO_ACCESS_GENERATED_DIR}
    ${CUOBJCLIENT_INCLUDE_DIR}
    ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_access_client
  PUBLIC
    us3_turbo_access_common
    us3_turbo_access_spdlog
    nlohmann_json::nlohmann_json
    us3_turbo_access_brpc
    ${IBVERBS_LIBRARY}
    ${RDMACM_LIBRARY}
    ${CUOBJSERVER_LIBRARY}
    ${CUDART_LIBRARY})

if(UNIX AND NOT APPLE)
  target_link_libraries(us3_turbo_access_client PRIVATE dl)
endif()

target_compile_options(us3_turbo_access_client
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
