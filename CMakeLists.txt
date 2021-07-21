cmake_minimum_required(VERSION 3.16)
project(miximus)

set(CMAKE_CXX_STANDARD 17)
#set(MIXIMUS_CLANG_TIDY "clang-tidy;-checks=*,-fuchsia-default-arguments-calls,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-magic-numbers, -cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-pro-type-reinterpret-cast,-google-build-using-namespace,-modernize-use-nodiscard,-fuchsia-overloaded-operator,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay")

set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}
    CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel tsan asan lsan msan ubsan"
    FORCE)

set(CMAKE_CXX_FLAGS_TSAN
    "-fsanitize=thread -g -O1"
    CACHE STRING "Flags used by the C++ compiler during ThreadSanitizer builds."
    FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(ZLIB REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(CUDA REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(websocketpp REQUIRED)
find_package(Threads REQUIRED)

add_definitions(-D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN)

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_DEBUG_LIBS OFF)
set(Boost_USE_RELEASE_LIBS ON)
find_package(Boost COMPONENTS system REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/frozen/include
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