cmake_minimum_required(VERSION 3.19)
set(ProjectName miximus)
project(${ProjectName})

set (CMAKE_CXX_STANDARD 17)

set(QUIC_BUILD_TOOLS OFF)
set(QUIC_BUILD_TEST OFF)
set(QUIC_BUILD_PERF OFF)
add_subdirectory(submodules/msquic)

set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
add_subdirectory(submodules/glfw)
add_subdirectory(submodules/glad)

add_subdirectory(submodules/json)
add_subdirectory(submodules/fmt)
add_subdirectory(submodules/zlib)

add_subdirectory(static)
add_subdirectory(src)

set_target_properties(${ProjectName}
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )