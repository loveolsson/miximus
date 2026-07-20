add_library(render)

add_subdirectory(font)
add_subdirectory(surface)
add_subdirectory(test_pattern)

target_link_libraries(render
PRIVATE
    logger
    gpu
    miximus_types
    static_files
    stb
)

add_sanitizers(render)
