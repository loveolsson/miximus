include(FindPackageHandleStandardArgs)

add_library(ffmpeg INTERFACE)

macro(find_component _library _header)
  find_path(INCLUDE_DIRS ${_header} REQUIRED)

  find_library(LIBRARIES NAMES ${_library} REQUIRED)

  target_include_directories(ffmpeg INTERFACE ${INCLUDE_DIRS})
  target_link_libraries(ffmpeg INTERFACE ${LIBRARIES})

  message("-- Found ${_library}: ${LIBRARIES}, ${INCLUDE_DIRS}")

endmacro()

find_component(avcodec    libavcodec/avcodec.h)
find_component(avformat   libavformat/avformat.h)
find_component(avdevice   libavdevice/avdevice.h)
find_component(avutil     libavutil/avutil.h)
find_component(avfilter   libavfilter/avfilter.h)
find_component(swscale    libswscale/swscale.h)
find_component(postproc   libpostproc/postprocess.h)
find_component(swresample libswresample/swresample.h)