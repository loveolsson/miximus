add_library(render
    surface.cpp
    surface.hpp
)

target_link_libraries(render
PRIVATE
    logger
)

add_subdirectory(font)

add_sanitizers(render)