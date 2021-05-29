cmake_minimum_required(VERSION 3.16)
project(miximus)

set(CMAKE_CXX_STANDARD 17)

find_package(ZLIB REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(websocketpp REQUIRED)
find_package(Threads REQUIRED)

include(cmake/find-ffmpeg.cmake)

message("${FFMPEG_INCLUDE_DIRS}")


set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_DEBUG_LIBS OFF)
set(Boost_USE_RELEASE_LIBS ON)
find_package(Boost COMPONENTS system REQUIRED)

set(CEF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/3rd-party/cef)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CEF_ROOT}/cmake)

find_package(CEF REQUIRED)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
if(MSVC)
    target_compile_options(libcef_dll_wrapper PRIVATE
        $<$<CONFIG:>:/MD>
        $<$<CONFIG:Debug>:/MDd>
        $<$<CONFIG:Release>:/MD>
    )
endif()

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
