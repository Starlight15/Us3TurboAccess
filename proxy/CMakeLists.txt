add_executable(us3_turbo_access_proxy
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  src/main.cpp
  src/proxy_control_plane_service.cpp
  src/session/session_manager.cpp)

add_dependencies(us3_turbo_access_proxy
  us3_turbo_access_control_plane_proto)

target_include_directories(us3_turbo_access_proxy
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
    ${US3_TURBO_ACCESS_GENERATED_DIR})

target_link_libraries(us3_turbo_access_proxy
  PRIVATE
    us3_turbo_access_common
    us3_turbo_access_spdlog
    us3_turbo_access_brpc)

target_compile_options(us3_turbo_access_proxy
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
