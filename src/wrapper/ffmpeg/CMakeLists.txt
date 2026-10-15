include(SelectLibraryConfigurations)

add_library(ffmpeg INTERFACE)

add_library(ffmpeg_platform STATIC hardware.hpp)
if(WIN32)
  target_sources(ffmpeg_platform PRIVATE hardware_win.cpp)
elseif(APPLE)
  target_sources(ffmpeg_platform PRIVATE hardware_mac.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(ffmpeg_platform PRIVATE hardware_linux.cpp)
else()
  target_sources(ffmpeg_platform PRIVATE hardware_other.cpp)
endif()
target_link_libraries(ffmpeg_platform PUBLIC ffmpeg)

function(find_ffmpeg_component _library _header)
  set(component "FFMPEG_${_library}")
  find_path(${component}_INCLUDE_DIR NAMES ${_header} REQUIRED)
  if(WIN32 AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    # vcpkg puts debug/ first in the search path for Debug configurations.
    # Search each configuration explicitly so the first configured variant does
    # not determine which DLL import library all later variants use.
    set(library_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    string(TOLOWER "${${component}_LIBRARY_RELEASE}" cached_release)
    string(TOLOWER "${library_root}/debug/lib/${_library}.lib" debug_library)
    if(cached_release STREQUAL debug_library)
      # Repair only the erroneous entry produced by the former discovery code.
      unset(${component}_LIBRARY_RELEASE CACHE)
      unset(${component}_LIBRARY_RELEASE)
    endif()
    find_library(${component}_LIBRARY_RELEASE NAMES ${_library}
      PATHS "${library_root}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(${component}_LIBRARY_DEBUG NAMES ${_library}
      PATHS "${library_root}/debug/lib" NO_DEFAULT_PATH REQUIRED)
  else()
    find_library(${component}_LIBRARY_RELEASE NAMES ${_library} REQUIRED)
  endif()

  if(WIN32 AND NOT (VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET))
    # Support other SDKs with the same separate Debug import-library layout.
    cmake_path(GET ${component}_LIBRARY_RELEASE PARENT_PATH library_dir)
    find_library(${component}_LIBRARY_DEBUG NAMES ${_library}
      PATHS "${library_dir}/../debug/lib" NO_DEFAULT_PATH)
  endif()
  select_library_configurations(${component})

  target_include_directories(ffmpeg SYSTEM INTERFACE "${${component}_INCLUDE_DIR}")
  if(WIN32 AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    # Imported shared targets make FFmpeg DLLs visible to the existing staging
    # machinery, including test discovery before the application is linked.
    file(GLOB runtime_release "${library_root}/bin/${_library}-*.dll")
    file(GLOB runtime_debug "${library_root}/debug/bin/${_library}-*.dll")
    list(LENGTH runtime_release release_count)
    list(LENGTH runtime_debug debug_count)
    if(NOT release_count EQUAL 1 OR NOT debug_count EQUAL 1)
      message(FATAL_ERROR "Expected one FFmpeg ${_library} runtime per configuration")
    endif()
    add_library(ffmpeg_${_library} SHARED IMPORTED GLOBAL)
    set_target_properties(ffmpeg_${_library} PROPERTIES
      IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
      IMPORTED_IMPLIB_RELEASE "${${component}_LIBRARY_RELEASE}"
      IMPORTED_IMPLIB_DEBUG "${${component}_LIBRARY_DEBUG}"
      IMPORTED_LOCATION_RELEASE "${runtime_release}"
      IMPORTED_LOCATION_DEBUG "${runtime_debug}"
      MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
      MAP_IMPORTED_CONFIG_MINSIZEREL Release)
    target_link_libraries(ffmpeg INTERFACE ffmpeg_${_library})
  else()
    target_link_libraries(ffmpeg INTERFACE ${${component}_LIBRARY})
  endif()

  message(STATUS "Found ${_library}: ${${component}_LIBRARY}, ${${component}_INCLUDE_DIR}")
  mark_as_advanced(${component}_INCLUDE_DIR ${component}_LIBRARY_RELEASE ${component}_LIBRARY_DEBUG)
endfunction()

# Discover only components consumed by playback. Add device/filter/audio
# processing libraries when an implementation actually requires them.
find_ffmpeg_component(avcodec    libavcodec/avcodec.h)
find_ffmpeg_component(avformat   libavformat/avformat.h)
find_ffmpeg_component(avutil     libavutil/avutil.h)
find_ffmpeg_component(swscale    libswscale/swscale.h)
