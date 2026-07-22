add_library(miximus_media
    source_clock.hpp
    source_clock.cpp
    timed_source_queue.hpp
)

target_include_directories(miximus_media PUBLIC ${PROJECT_SOURCE_DIR}/src)

add_sanitizers(miximus_media)
