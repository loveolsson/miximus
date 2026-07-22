cmake_minimum_required(VERSION 3.28)
project(miximus)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/submodules/sanitizers-cmake/cmake")

option(MIXIMUS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling Miximus targets" OFF)
option(MIXIMUS_ENABLE_PRECOMPILED_HEADERS "Use precompiled headers for expensive common dependencies" ON)

set(MIXIMUS_USE_PRECOMPILED_HEADERS ${MIXIMUS_ENABLE_PRECOMPILED_HEADERS})
if(MIXIMUS_ENABLE_PRECOMPILED_HEADERS AND MIXIMUS_ENABLE_CLANG_TIDY)
    set(MIXIMUS_USE_PRECOMPILED_HEADERS OFF)
    message(STATUS "Precompiled headers disabled while clang-tidy is enabled")
endif()

if(MIXIMUS_ENABLE_CLANG_TIDY)
    find_program(MIXIMUS_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    message(STATUS "clang-tidy enabled: ${MIXIMUS_CLANG_TIDY_EXECUTABLE}")
endif()

find_package(Sanitizers REQUIRED)
find_package(Threads REQUIRED)

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_DEBUG_LIBS ON)
set(Boost_USE_RELEASE_LIBS ON)
find_package(Boost REQUIRED COMPONENTS fiber url CONFIG)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/fiberpool/include
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

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(submodules)

if(MIXIMUS_ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "${MIXIMUS_CLANG_TIDY_EXECUTABLE}")
endif()

# Strict warnings — applied only to our own targets, after submodules are added.
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
)

add_subdirectory(src)
add_subdirectory(static)

set_target_properties(miximus
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

# Print a prominent warning at the end of the build if the Web UI failed to build.
set(WEB_BUILD_FAILED_FILE "${CMAKE_BINARY_DIR}/static/web_build_failed.txt")
add_custom_target(report_build_summary ALL
  COMMAND ${CMAKE_COMMAND}
    -DWEB_BUILD_FAILED_FILE=${WEB_BUILD_FAILED_FILE}
    -P "${CMAKE_SOURCE_DIR}/cmake/report_build_summary.cmake"
  VERBATIM
)
add_dependencies(report_build_summary miximus)

# if(CMAKE_BUILD_TYPE STREQUAL "Release")
#     include(CheckIPOSupported)
#     check_ipo_supported(RESULT result)
#     if(result)
#         set_property(TARGET miximus libcef_dll_wrapper PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#     else()
#         message(WARNING "IPO is not supported: ${output}")
#     endif()
# endif()
