add_library(ndi INTERFACE)

set(NDI_ROOT "" CACHE PATH "Root directory of the NDI 6.2 SDK")

set(NDI_ROOT_HINTS
    "${NDI_ROOT}"
    "${NDI_SDK_DIR}"
    "$ENV{NDI_SDK_DIR}"
    "$ENV{NDI_ROOT}"
)

if(WIN32)
    list(APPEND NDI_ROOT_HINTS
        "$ENV{PROGRAMFILES}/NDI/NDI 6 SDK"
        "$ENV{PROGRAMFILES}/NDI/NDI SDK"
    )
elseif(APPLE)
    list(APPEND NDI_ROOT_HINTS
        "/Library/NDI SDK for Apple"
        "/Library/NDI SDK for macOS"
    )
else()
    list(APPEND NDI_ROOT_HINTS
        "/usr/local/lib/NDI SDK for Linux"
        "/usr/lib/NDI SDK for Linux"
    )
endif()

find_path(NDI_INCLUDE_DIR
    NAMES Processing.NDI.Lib.h
    HINTS ${NDI_ROOT_HINTS}
    PATH_SUFFIXES include Include
    REQUIRED
)

cmake_path(GET NDI_INCLUDE_DIR PARENT_PATH NDI_DISCOVERED_ROOT)

set(NDI_LIBRARY_NAMES ndi)
set(NDI_LIBRARY_SUFFIXES
    lib/${CMAKE_LIBRARY_ARCHITECTURE}
    lib
    Lib
)

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(PREPEND NDI_LIBRARY_NAMES Processing.NDI.Lib.x64)
        list(PREPEND NDI_LIBRARY_SUFFIXES Lib/x64)
    else()
        list(PREPEND NDI_LIBRARY_NAMES Processing.NDI.Lib.x86)
        list(PREPEND NDI_LIBRARY_SUFFIXES Lib/x86)
    endif()
elseif(APPLE)
    list(PREPEND NDI_LIBRARY_SUFFIXES lib/macOS)
endif()

find_library(NDI_LIBRARY
    NAMES ${NDI_LIBRARY_NAMES}
    PATHS "${NDI_DISCOVERED_ROOT}"
    PATH_SUFFIXES ${NDI_LIBRARY_SUFFIXES}
    NO_DEFAULT_PATH
    REQUIRED
)

set(NDI_VERSION_FILE "${NDI_DISCOVERED_ROOT}/Version.txt")
if(NOT EXISTS "${NDI_VERSION_FILE}")
    message(FATAL_ERROR "Could not determine the NDI SDK version: ${NDI_VERSION_FILE} does not exist")
endif()

file(STRINGS "${NDI_VERSION_FILE}" NDI_VERSION_LINE LIMIT_COUNT 1)
string(REGEX MATCH "v([0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?)" NDI_VERSION_MATCH "${NDI_VERSION_LINE}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Could not parse the NDI SDK version from ${NDI_VERSION_FILE}")
endif()

set(NDI_VERSION "${CMAKE_MATCH_1}")
if(NDI_VERSION VERSION_LESS 6.2)
    message(FATAL_ERROR "NDI 6.2 or newer is required, but ${NDI_DISCOVERED_ROOT} contains NDI ${NDI_VERSION}")
endif()

target_include_directories(ndi SYSTEM INTERFACE "${NDI_INCLUDE_DIR}")
target_link_libraries(ndi INTERFACE "${NDI_LIBRARY}")

message(STATUS "Found NDI ${NDI_VERSION}: ${NDI_LIBRARY}, ${NDI_INCLUDE_DIR}")

mark_as_advanced(NDI_INCLUDE_DIR NDI_LIBRARY)
