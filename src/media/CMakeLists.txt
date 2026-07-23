add_library(media
    media_frame.hpp
    source_clock.hpp
    source_clock.cpp
    timed_source_queue.hpp
)

target_include_directories(media PUBLIC ${PROJECT_SOURCE_DIR}/src)

add_sanitizers(media)
