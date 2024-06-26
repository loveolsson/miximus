cmake_minimum_required(VERSION 3.16)
project(miximus)

set(CMAKE_CXX_STANDARD 20)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/submodules/sanitizers-cmake/cmake")

set(MIXIMUS_CLANG_TIDY_PARAMS
    -checks=*
    -fuchsia-default-arguments-calls
    -modernize-use-trailing-return-type
    -cppcoreguidelines-pro-bounds-pointer-arithmetic
    -cppcoreguidelines-avoid-magic-numbers
    -readability-magic-numbers
    -cppcoreguidelines-pro-type-reinterpret-cast
    -google-build-using-namespace
    -modernize-use-nodiscard
    -fuchsia-overloaded-operator
    -cppcoreguidelines-pro-bounds-array-to-pointer-decay
    -hicpp-no-array-decay
    -cppcoreguidelines-pro-type-union-access
    -fuchsia-statically-constructed-objects
    -cppcoreguidelines-no-malloc
    -hicpp-no-malloc
    -cppcoreguidelines-owning-memory
    -cppcoreguidelines-macro-usage
    -cppcoreguidelines-pro-type-vararg
    -hicpp-vararg
    -misc-non-private-member-variables-in-classes
    -clang-analyzer-cplusplus.NewDelete
    -llvmlibc*
    -altera*
    -readability-identifier-length
    -abseil*
    -bugprone-easily-swappable-parameters
    -readability-container-data-pointer,
    -readability-qualified-auto,
    -llvm-qualified-auto
)

list(JOIN MIXIMUS_CLANG_TIDY_PARAMS "," MIXIMUS_CLANG_TIDY_PARAMS_STR)
#set(MIXIMUS_CLANG_TIDY "clang-tidy;${MIXIMUS_CLANG_TIDY_PARAMS_STR}")

find_package(Sanitizers REQUIRED)
find_package(Threads REQUIRED)

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_DEBUG_LIBS ON)
set(Boost_USE_RELEASE_LIBS ON)
find_package(Boost REQUIRED COMPONENTS system fiber)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/frozen/include
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/concurrentqueue
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

add_subdirectory(submodules)
add_subdirectory(src)
add_subdirectory(static)

set_target_properties(miximus
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

# if(CMAKE_BUILD_TYPE STREQUAL "Release")
#     include(CheckIPOSupported)
#     check_ipo_supported(RESULT result)
#     if(result)
#         set_property(TARGET miximus libcef_dll_wrapper PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#     else()
#         message(WARNING "IPO is not supported: ${output}")
#     endif()
# endif()