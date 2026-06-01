# bench/ — 性能压测代码
#
# 编译产物：
#   build/bench/us3_turbo_access_<path>_<mode>_bench
#
# 与 examples/ 平行：examples/ 关心功能正确性，bench/ 关心吞吐/延迟数字。
# 日志和 CSV 输出统一到 bench/logs/。

# HTTP path
add_executable(us3_turbo_access_http_put_bench       http_put_bench.cpp)
add_executable(us3_turbo_access_http_multipart_bench http_multipart_bench.cpp)

# RDMA path
add_executable(us3_turbo_access_rdma_put_bench       rdma_put_bench.cpp)
add_executable(us3_turbo_access_rdma_multipart_bench rdma_multipart_bench.cpp)

# GDS path（v1 已弃用但保留兼容；v2 是当前推荐版本）
add_executable(us3_turbo_access_gds_put_bench        gds_put_bench.cpp)
add_executable(us3_turbo_access_gds_put_bench_v2     gds_put_bench_v2.cpp)
add_executable(us3_turbo_access_gds_multipart_bench  gds_multipart_bench.cpp)

set(US3_BENCH_TARGETS
    us3_turbo_access_http_put_bench
    us3_turbo_access_http_multipart_bench
    us3_turbo_access_rdma_put_bench
    us3_turbo_access_rdma_multipart_bench
    us3_turbo_access_gds_put_bench
    us3_turbo_access_gds_put_bench_v2
    us3_turbo_access_gds_multipart_bench
)
foreach(_t IN LISTS US3_BENCH_TARGETS)
  target_link_libraries(${_t} PRIVATE Us3TurboAccess::client ${CUDART_LIBRARY})
endforeach()

# bench 用例普遍读项目根的 client/src 内部头（http_crc32c 等共享工具）。
target_include_directories(us3_turbo_access_http_put_bench       PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(us3_turbo_access_http_multipart_bench PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(us3_turbo_access_rdma_put_bench       PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(us3_turbo_access_rdma_multipart_bench PRIVATE ${PROJECT_SOURCE_DIR})

target_include_directories(us3_turbo_access_gds_put_bench        PRIVATE ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_gds_put_bench_v2     PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_gds_multipart_bench  PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
