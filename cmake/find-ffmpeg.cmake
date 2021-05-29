include(FindPackageHandleStandardArgs)

macro(set_component_found _component )
  if (${_component}_LIBRARIES AND ${_component}_INCLUDE_DIRS)
    # message(STATUS "  - ${_component} found.")
    set(${_component}_FOUND TRUE)
  else ()
    # message(STATUS "  - ${_component} not found.")
  endif ()
endmacro()

set (FFMPEG_INCLUDE_DIRS "")

#
### Macro: find_component
#
# Checks for the given component by invoking pkgconfig and then looking up the libraries and
# include directories.
#
macro(find_component _component _pkgconfig _library _header)
  find_path(${_component}_INCLUDE_DIRS ${_header}
    HINTS
      ${PC_LIB${_component}_INCLUDEDIR}
      ${PC_LIB${_component}_INCLUDE_DIRS}
    PATH_SUFFIXES
      ffmpeg
    REQUIRED
  )

  find_library(${_component}_LIBRARIES NAMES ${_library}
      HINTS
      ${PC_LIB${_component}_LIBDIR}
      ${PC_LIB${_component}_LIBRARY_DIRS}
    REQUIRED
  )

  set(${_component}_DEFINITIONS  ${PC_${_component}_CFLAGS_OTHER} CACHE STRING "The ${_component} CFLAGS.")
  set(${_component}_VERSION      ${PC_${_component}_VERSION}      CACHE STRING "The ${_component} version number.")

  set_component_found(${_component})

  mark_as_advanced(
    ${_component}_LIBRARY_DIRS
    ${_component}_INCLUDE_DIRS
    ${_component}_LIBRARIES
    ${_component}_DEFINITIONS
    ${_component}_VERSION
  )

  list(APPEND FFMPEG_INCLUDE_DIRS ${${_component}_INCLUDE_DIRS})

  message("Found ${_component} ${${_component}_LIBRARIES}")

endmacro()

# Check for all possible component.
find_component(AVCODEC    libavcodec    avcodec  libavcodec/avcodec.h)
find_component(AVFORMAT   libavformat   avformat libavformat/avformat.h)
find_component(AVDEVICE   libavdevice   avdevice libavdevice/avdevice.h)
find_component(AVUTIL     libavutil     avutil   libavutil/avutil.h)
find_component(AVFILTER   libavfilter   avfilter libavfilter/avfilter.h)
find_component(SWSCALE    libswscale    swscale  libswscale/swscale.h)
find_component(POSTPROC   libpostproc   postproc libpostproc/postprocess.h)
find_component(SWRESAMPLE libswresample swresample libswresample/swresample.h)

list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
