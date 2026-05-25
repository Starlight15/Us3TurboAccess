add_executable(us3_turbo_access_gds_example
  gds_gateway_example.cpp)

add_executable(us3_turbo_access_multipart_example
  multipart_example.cpp)

add_executable(us3_turbo_access_gds_put_bench
  gds_put_bench.cpp)

add_executable(us3_turbo_access_cuobj_probe
  cuobj_probe.cpp)

add_executable(us3_turbo_access_env_probe
  env_probe.cpp)
target_include_directories(us3_turbo_access_env_probe
  PRIVATE
    ${CUFILE_INCLUDE_DIR})
target_link_libraries(us3_turbo_access_env_probe
  PRIVATE
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
