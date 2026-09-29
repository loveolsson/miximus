include(SelectLibraryConfigurations)

add_library(ffmpeg INTERFACE)

function(find_ffmpeg_component _library _header)
  set(component "FFMPEG_${_library}")
  find_path(${component}_INCLUDE_DIR NAMES ${_header} REQUIRED)
  find_library(${component}_LIBRARY_RELEASE NAMES ${_library} REQUIRED)

  if(WIN32)
    # vcpkg keeps Debug DLL import libraries separate from Release libraries.
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
find_ffmpeg_component(postproc   libpostproc/postprocess.h)
find_ffmpeg_component(swresample libswresample/swresample.h)
