# bench/ — 性能压测代码
#
# 编译产物：build/bench/us3_turbo_access_<path>_<mode>_bench
# 与 examples/ 平行：examples/ 关心功能正确性，bench/ 关心吞吐/延迟数字。

# RDMA/UCX path
add_executable(us3_turbo_access_rdma_put_bench       rdma_put_bench.cpp)
add_executable(us3_turbo_access_rdma_multipart_bench rdma_multipart_bench.cpp)

# GDS path
add_executable(us3_turbo_access_gds_put_bench        gds_put_bench_v2.cpp)
add_executable(us3_turbo_access_gds_multipart_bench  gds_multipart_bench.cpp)

set(US3_BENCH_TARGETS
    us3_turbo_access_rdma_put_bench
    us3_turbo_access_rdma_multipart_bench
    us3_turbo_access_gds_put_bench
    us3_turbo_access_gds_multipart_bench
)
foreach(_t IN LISTS US3_BENCH_TARGETS)
  target_link_libraries(${_t} PRIVATE Us3TurboAccess::client ${CUDART_LIBRARY})
endforeach()

target_include_directories(us3_turbo_access_rdma_put_bench       PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(us3_turbo_access_rdma_multipart_bench PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(us3_turbo_access_gds_put_bench        PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_gds_multipart_bench  PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})

# UCX PoC — 独立于 client lib，用于协议基线验证
find_package(ucx CONFIG)
if(ucx_FOUND)
  add_executable(us3_turbo_access_ucx_put_poc ucx_put_poc.cpp)
  target_link_libraries(us3_turbo_access_ucx_put_poc PRIVATE ucx::ucp ucx::ucs)
  message(STATUS "UCX found: enabling ucx_put_poc")
else()
  message(STATUS "UCX not found: skipping ucx_put_poc")
endif()
