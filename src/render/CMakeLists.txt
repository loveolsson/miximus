add_library(render)

add_subdirectory(font)
add_subdirectory(surface)

target_link_libraries(render
PRIVATE
    logger
    gpu
)

add_sanitizers(render)