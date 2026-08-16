add_library(media
    media_clock_sample.hpp
    output_runtime_metrics.hpp
    presentation_timeline.hpp
    media_clock.hpp
    media_clock.cpp
    timed_output_queue.hpp
    timed_source_queue.hpp
)

target_include_directories(media PUBLIC ${PROJECT_SOURCE_DIR}/src)

add_sanitizers(media)
