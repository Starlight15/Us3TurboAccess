add_executable(us3_turbo_access_gds_example
  gds_gateway_example.cpp)

add_executable(us3_turbo_access_multipart_example
  multipart_example.cpp)

add_executable(us3_turbo_access_gds_put_bench
  gds_put_bench.cpp)

add_executable(us3_turbo_access_gds_put_bench_v2
  gds_put_bench_v2.cpp)
target_include_directories(us3_turbo_access_gds_put_bench_v2
  PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_gds_put_bench_v2
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_gds_multipart_bench
  gds_multipart_bench.cpp)
target_include_directories(us3_turbo_access_gds_multipart_bench
  PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_gds_multipart_bench
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_cuobj_probe
  cuobj_probe.cpp)

add_executable(us3_turbo_access_gds_token_probe
  gds_token_probe.cpp)
target_include_directories(us3_turbo_access_gds_token_probe
  PRIVATE ${PROJECT_SOURCE_DIR} ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_gds_token_probe
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_env_probe
  env_probe.cpp)
target_include_directories(us3_turbo_access_env_probe
  PRIVATE
    ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_env_probe
  PRIVATE
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_rdma_put_example
  rdma_put_example.cpp)
target_link_libraries(us3_turbo_access_rdma_put_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_rdma_put_async_example
  rdma_put_async_example.cpp)
target_link_libraries(us3_turbo_access_rdma_put_async_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_rdma_multipart_example
  rdma_multipart_example.cpp)
target_link_libraries(us3_turbo_access_rdma_multipart_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_rdma_put_bench
  rdma_put_bench.cpp)
target_include_directories(us3_turbo_access_rdma_put_bench
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(us3_turbo_access_rdma_put_bench
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_rdma_multipart_bench
  rdma_multipart_bench.cpp)
target_include_directories(us3_turbo_access_rdma_multipart_bench
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(us3_turbo_access_rdma_multipart_bench
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_http_put_example
  http_put_example.cpp)
target_link_libraries(us3_turbo_access_http_put_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_http_multipart_example
  http_multipart_example.cpp)
target_link_libraries(us3_turbo_access_http_multipart_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_http_parallel_get_example
  http_parallel_get_example.cpp)
target_link_libraries(us3_turbo_access_http_parallel_get_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_http_verify_example
  http_verify_example.cpp)
target_include_directories(us3_turbo_access_http_verify_example
  PRIVATE
    ${PROJECT_SOURCE_DIR})
target_link_libraries(us3_turbo_access_http_verify_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_http_put_bench
  http_put_bench.cpp)
target_include_directories(us3_turbo_access_http_put_bench
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(us3_turbo_access_http_put_bench
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_http_multipart_bench
  http_multipart_bench.cpp)
target_include_directories(us3_turbo_access_http_multipart_bench
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(us3_turbo_access_http_multipart_bench
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

add_executable(us3_turbo_access_rdma_probe
  rdma_probe.cpp)
target_include_directories(us3_turbo_access_rdma_probe
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/client/include
    ${PROJECT_SOURCE_DIR}/client/src
    ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_rdma_probe
  PRIVATE
    Us3TurboAccess::client
    ${IBVERBS_LIBRARY}
    ${CUDART_LIBRARY})

target_include_directories(us3_turbo_access_gds_example
  PRIVATE
    ${CUFILE_INCLUDE_DIR})

target_include_directories(us3_turbo_access_multipart_example
  PRIVATE
    ${CUFILE_INCLUDE_DIR})

target_include_directories(us3_turbo_access_gds_put_bench
  PRIVATE
    ${CUFILE_INCLUDE_DIR})

target_include_directories(us3_turbo_access_cuobj_probe
  PRIVATE
    ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_access_gds_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

target_link_libraries(us3_turbo_access_multipart_example
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

target_link_libraries(us3_turbo_access_gds_put_bench
  PRIVATE
    Us3TurboAccess::client
    ${CUDART_LIBRARY})

target_link_libraries(us3_turbo_access_cuobj_probe
  PRIVATE
    ${CUDART_LIBRARY}
    dl)
