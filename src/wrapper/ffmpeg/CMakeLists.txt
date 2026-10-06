include(SelectLibraryConfigurations)

add_library(ffmpeg INTERFACE)

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
  target_link_libraries(ffmpeg INTERFACE ${${component}_LIBRARY})

  message(STATUS "Found ${_library}: ${${component}_LIBRARY}, ${${component}_INCLUDE_DIR}")
  mark_as_advanced(${component}_INCLUDE_DIR ${component}_LIBRARY_RELEASE ${component}_LIBRARY_DEBUG)
endfunction()

find_ffmpeg_component(avcodec    libavcodec/avcodec.h)
find_ffmpeg_component(avformat   libavformat/avformat.h)
find_ffmpeg_component(avdevice   libavdevice/avdevice.h)
find_ffmpeg_component(avutil     libavutil/avutil.h)
find_ffmpeg_component(avfilter   libavfilter/avfilter.h)
find_ffmpeg_component(swscale    libswscale/swscale.h)
find_ffmpeg_component(swresample libswresample/swresample.h)
