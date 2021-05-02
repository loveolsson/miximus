set(QUIC_BUILD_TOOLS OFF)
set(QUIC_BUILD_TEST OFF)
set(QUIC_BUILD_PERF OFF)
add_subdirectory(msquic)

set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
add_subdirectory(glfw)
add_subdirectory(glad)

add_subdirectory(zlib)
add_subdirectory(spdlog)