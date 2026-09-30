cmake_minimum_required(VERSION 3.28)
project(miximus)

include(CTest)

if(BUILD_TESTING)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(submodules/googletest EXCLUDE_FROM_ALL)
    include(GoogleTest)
endif()

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
find_package(Boost REQUIRED COMPONENTS fiber program_options url CONFIG)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/fiberpool/include
)

option(MIXIMUS_TUNE_NATIVE "Enable architecture-specific compiler tuning" OFF)

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
    set(MIXIMUS_CLANG_TIDY_COMMAND "${MIXIMUS_CLANG_TIDY_EXECUTABLE}")
    if(MSVC)
        # Clang tooling drops the compiler's /EHsc; retain C++ exception semantics.
        list(APPEND MIXIMUS_CLANG_TIDY_COMMAND "--extra-arg=/EHsc")
    endif()
    set(CMAKE_CXX_CLANG_TIDY "${MIXIMUS_CLANG_TIDY_COMMAND}")
endif()

# Strict warnings — applied only to our own targets, after submodules are added.
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
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
endif()

function(miximus_stage_runtime_dlls runtime_target)
  if(WIN32)
    # Stage linked project/SDK DLLs before POST_BUILD GoogleTest discovery.
    # vcpkg additionally stages transitive DLL imports from its installed tree.
    add_custom_command(TARGET ${runtime_target} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${runtime_target}> $<TARGET_FILE_DIR:${runtime_target}>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
  endif()
endfunction()

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

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/release_ipo.cmake")
