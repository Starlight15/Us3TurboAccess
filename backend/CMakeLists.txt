add_executable(us3_turbo_access_backend
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  src/main.cpp
  src/backend_gds_sink.cpp
  src/backend_data_plane_service.cpp
  # 复用 gateway 源文件（不改，按路径编译进 backend target）：
  #   buffer_pool.cpp / cuobj_resources.cpp —— cuObjServer pinned buffer 池与 RAII
  #   common/crc32c.cpp —— 自包含 CRC32C，供 PUT 丢弃前算校验和做端到端校验
  ${PROJECT_SOURCE_DIR}/gateway/src/data_path/gds/buffer_pool.cpp
  ${PROJECT_SOURCE_DIR}/gateway/src/data_path/gds/cuobj_resources.cpp
  ${PROJECT_SOURCE_DIR}/gateway/src/common/crc32c.cpp)

add_dependencies(us3_turbo_access_backend
  us3_turbo_access_control_plane_proto)

target_include_directories(us3_turbo_access_backend
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/gateway/src
    ${US3_TURBO_ACCESS_GENERATED_DIR}
    ${CUOBJCLIENT_INCLUDE_DIR}
    ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_access_backend
  PRIVATE
    us3_turbo_access_common
    us3_turbo_access_spdlog
    us3_turbo_access_brpc
    ${CUOBJSERVER_LIBRARY}
    ${IBVERBS_LIBRARY}
    ${CUDART_LIBRARY})

if(UNIX AND NOT APPLE)
  target_link_libraries(us3_turbo_access_backend PRIVATE dl)
endif()

target_compile_options(us3_turbo_access_backend
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
