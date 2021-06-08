cmake_minimum_required(VERSION 3.16)
project(miximus)

set(CMAKE_CXX_STANDARD 17)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(ZLIB REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(CUDA REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(websocketpp REQUIRED)
find_package(Threads REQUIRED)

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_DEBUG_LIBS OFF)
set(Boost_USE_RELEASE_LIBS ON)
find_package(Boost COMPONENTS system REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/eternal/include
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/stb
)

option(MIXIMUS_TUNE_NATIVE OFF)

if (MIXIMUS_TUNE_NATIVE)
    include(CheckCXXCompilerFlag)
    message("Attempting to set tune native")
    CHECK_CXX_COMPILER_FLAG("-march=native" MARCHNATIVE)
    CHECK_CXX_COMPILER_FLAG("-mtune=native" MTUNENATIVE)

    if (MARCHNATIVE)
        message("- Setting march")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
    endif()

    if (MTUNENATIVE)
        message("- Setting mtune")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mtune=native")
    endif()
endif()

add_subdirectory(submodules)
add_subdirectory(src)
add_subdirectory(static)

set_target_properties(miximus
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT result)
    if(result)
        set_property(TARGET miximus libcef_dll_wrapper PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO is not supported: ${output}")
    endif()
endif()