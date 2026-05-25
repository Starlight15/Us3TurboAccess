add_executable(us3_turbo_access_gateway
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  ${US3_TURBO_ACCESS_HTTP_PROTO_SRCS}
  src/app/main.cpp
  src/app/server.cpp
  src/api/control_plane_service.cpp
  src/api/http_frontend.cpp
  src/backend/composite_backend.cpp
  src/backend/memory_data_store.cpp
  src/backend/memory_index_store.cpp
  src/backend/null_backend.cpp
  src/backend/null_data_store.cpp
  src/backend/null_index_store.cpp
  src/common/error.cpp
  src/common/ids.cpp
  src/common/log.cpp
  src/common/metrics.cpp
  src/common/range.cpp
  src/common/types.cpp
  src/core/session/session_store.cpp
  src/core/session/session_sweeper.cpp
  src/core/session_opener/session_opener.cpp
  src/core/multipart/multipart_store.cpp
  src/core/multipart/multipart_coordinator.cpp
  src/data_path/data_path_executor.cpp
  src/data_path/gds/buffer_pool.cpp
  src/data_path/gds/cuobj_resources.cpp
  src/data_path/gds/gds_executor.cpp
  src/data_path/http/http_executor.cpp
  src/runtime/gateway_runtime.cpp
  src/runtime/io_worker_pool.cpp)

add_dependencies(us3_turbo_access_gateway
  us3_turbo_access_control_plane_proto
  us3_turbo_access_http_gateway_proto)

target_include_directories(us3_turbo_access_gateway
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${US3_TURBO_ACCESS_GENERATED_DIR}
    ${CUOBJCLIENT_INCLUDE_DIR}
    ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_access_gateway
  PRIVATE
    us3_turbo_access_common
    us3_turbo_access_spdlog
    us3_turbo_access_brpc
    nlohmann_json::nlohmann_json
    ${CUOBJSERVER_LIBRARY}
    ${IBVERBS_LIBRARY}
    ${RDMACM_LIBRARY}
    ${CUDART_LIBRARY})

if(UNIX AND NOT APPLE)
  target_link_libraries(us3_turbo_access_gateway PRIVATE dl)
endif()

target_compile_options(us3_turbo_access_gateway
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
