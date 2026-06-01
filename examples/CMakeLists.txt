# examples/ — 功能验证 / 回归用例 + 环境探测工具
#
# 编译产物：
#   build/examples/us3_turbo_access_<name>_example
#   build/examples/us3_turbo_access_<name>_probe
#
# 性能压测代码不放这里，见 bench/CMakeLists.txt。

# ---- 功能用例 (examples/*.cpp) ----------------------------------------

# 共通：单线程示例 / 跨通路 multipart 示例
add_executable(us3_turbo_access_gds_example       gds_gateway_example.cpp)
add_executable(us3_turbo_access_multipart_example multipart_example.cpp)

# HTTP path 用例
add_executable(us3_turbo_access_http_put_example          http_put_example.cpp)
add_executable(us3_turbo_access_http_multipart_example    http_multipart_example.cpp)
add_executable(us3_turbo_access_http_parallel_get_example http_parallel_get_example.cpp)
add_executable(us3_turbo_access_http_verify_example       http_verify_example.cpp)

# RDMA path 用例
add_executable(us3_turbo_access_rdma_put_example       rdma_put_example.cpp)
add_executable(us3_turbo_access_rdma_put_async_example rdma_put_async_example.cpp)
add_executable(us3_turbo_access_rdma_multipart_example rdma_multipart_example.cpp)

set(US3_EXAMPLE_TARGETS
    us3_turbo_access_gds_example
    us3_turbo_access_multipart_example
    us3_turbo_access_http_put_example
    us3_turbo_access_http_multipart_example
    us3_turbo_access_http_parallel_get_example
    us3_turbo_access_http_verify_example
    us3_turbo_access_rdma_put_example
    us3_turbo_access_rdma_put_async_example
    us3_turbo_access_rdma_multipart_example
)
foreach(_t IN LISTS US3_EXAMPLE_TARGETS)
  target_link_libraries(${_t} PRIVATE Us3TurboAccess::client ${CUDART_LIBRARY})
endforeach()

# 个别用例需要项目根的 include 路径（client/src/data/http_crc32c.h 等内部头）
# 或 cuFile / CUDA 头
target_include_directories(us3_turbo_access_gds_example       PRIVATE ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_multipart_example PRIVATE ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_http_verify_example PRIVATE ${PROJECT_SOURCE_DIR})

# ---- 探测工具 (examples/probe/*.cpp) ----------------------------------

add_executable(us3_turbo_access_cuobj_probe     probe/cuobj_probe.cpp)
add_executable(us3_turbo_access_env_probe       probe/env_probe.cpp)
add_executable(us3_turbo_access_gds_token_probe probe/gds_token_probe.cpp)
add_executable(us3_turbo_access_rdma_probe      probe/rdma_probe.cpp)

target_include_directories(us3_turbo_access_cuobj_probe PRIVATE ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_cuobj_probe PRIVATE ${CUDART_LIBRARY} dl)

target_include_directories(us3_turbo_access_env_probe PRIVATE ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_env_probe PRIVATE ${CUDART_LIBRARY})

target_include_directories(us3_turbo_access_gds_token_probe
  PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_gds_token_probe
  PRIVATE Us3TurboAccess::client ${CUDART_LIBRARY})

target_include_directories(us3_turbo_access_rdma_probe
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/client/include
    ${PROJECT_SOURCE_DIR}/client/src
    ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_rdma_probe
  PRIVATE Us3TurboAccess::client ${IBVERBS_LIBRARY} ${CUDART_LIBRARY})
