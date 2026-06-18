# examples/ — 功能验证 / 回归用例 + 环境探测工具
#
# 目录结构：
#   http/   — HTTP 通路 (put, multipart + 对应测试脚本 + 回归测试)
#   rdma/   — RDMA 通路 (put, put_async, multipart + 对应测试脚本 + rdma_probe)
#   gds/    — GDS 通路 (put, multipart + 对应测试脚本 + 环境 probes)
#
# 编译产物：
#   build/examples/us3_turbo_access_<transport>_<put|multipart>_example
#   build/examples/us3_turbo_access_<name>_probe
#
# 性能压测代码不放这里，见 bench/CMakeLists.txt。

# ---- HTTP 通路用例 (http/*.cpp) ----------------------------------------

add_executable(us3_turbo_access_http_put_example       http/http_put_example.cpp)
add_executable(us3_turbo_access_http_multipart_example http/http_multipart_example.cpp)

# ---- RDMA 通路用例 (rdma/*.cpp) ----------------------------------------

add_executable(us3_turbo_access_rdma_put_example       rdma/rdma_put_example.cpp)
add_executable(us3_turbo_access_rdma_multipart_example rdma/rdma_multipart_example.cpp)

# ---- GDS 通路用例 (gds/*.cpp) ------------------------------------------

add_executable(us3_turbo_access_gds_put_example       gds/gds_put_example.cpp)
add_executable(us3_turbo_access_gds_put_split_example gds/gds_put_split_example.cpp)
add_executable(us3_turbo_access_gds_multipart_example gds/gds_multipart_example.cpp)

target_include_directories(us3_turbo_access_gds_put_example       PRIVATE ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_gds_put_split_example PRIVATE ${CUFILE_INCLUDE_DIR})
target_include_directories(us3_turbo_access_gds_multipart_example PRIVATE ${CUFILE_INCLUDE_DIR})

# ---- 共通链接 ----------------------------------------------------------

set(US3_EXAMPLE_TARGETS
    us3_turbo_access_http_put_example
    us3_turbo_access_http_multipart_example
    us3_turbo_access_rdma_put_example
    us3_turbo_access_rdma_multipart_example
    us3_turbo_access_gds_put_example
    us3_turbo_access_gds_put_split_example
    us3_turbo_access_gds_multipart_example
)
foreach(_t IN LISTS US3_EXAMPLE_TARGETS)
  target_link_libraries(${_t} PRIVATE Us3TurboAccess::client ${CUDART_LIBRARY})
endforeach()

# ---- 探测工具 ----------------------------------------------------------

# GDS probes
add_executable(us3_turbo_access_cuobj_probe     gds/cuobj_probe.cpp)
add_executable(us3_turbo_access_cuda_env_probe  gds/cuda_env_probe.cpp)

target_include_directories(us3_turbo_access_cuobj_probe PRIVATE ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_cuobj_probe PRIVATE ${CUDART_LIBRARY} dl)

target_include_directories(us3_turbo_access_cuda_env_probe PRIVATE ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_cuda_env_probe PRIVATE ${CUDART_LIBRARY})
