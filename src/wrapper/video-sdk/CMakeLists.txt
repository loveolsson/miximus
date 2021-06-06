set(VIDEO_SDK_ROOT ${CMAKE_SOURCE_DIR}/3rd-party/video-sdk)

add_library(video-sdk INTERFACE)

find_path(VIDEO_SDK_INCLUDE nvcuvid.h HINTS ${VIDEO_SDK_ROOT}/Interface REQUIRED)
target_include_directories(video-sdk INTERFACE ${VIDEO_SDK_INCLUDE})

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CUVID_LIB ${VIDEO_SDK_ROOT}/Lib/x64/nvcuvid.lib)
        set(NVENCODEAPI_LIB ${VIDEO_SDK_ROOT}/Lib/x64/nvencodeapi.lib)
    else()
        set(CUVID_LIB ${VIDEO_SDK_ROOT}/Lib/Win32/nvcuvid.lib)
        set(NVENCODEAPI_LIB ${VIDEO_SDK_ROOT}/Lib/Win32/nvencodeapi.lib)
    endif()
else()
    find_library(CUVID_LIB nvcuvid PATHS ${VIDEO_SDK_ROOT}/Lib/linux REQUIRED)
    find_library(NVENCODEAPI_LIB nvidia-encode PATHS ${VIDEO_SDK_ROOT}/Lib/linux REQUIRED)
endif()

message("-- Found cuvid: ${CUVID_LIB}")
message("-- Found nvencodeapi: ${NVENCODEAPI_LIB}")

target_link_libraries(video-sdk INTERFACE
    ${CUVID_LIB}
    ${NVENCODEAPI_LIB}
)

