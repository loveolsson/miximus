add_library(gpu
    vertex_buffer.hpp
    vertex_buffer.cpp
    vertex_array.hpp
    vertex_array.cpp
    context.hpp
    context.cpp
    shader.hpp
    shader.cpp
    framebuffer.hpp
    framebuffer.cpp
    vertex.hpp
    vertex_attr.hpp
    sync.hpp
    sync.cpp
    texture.hpp
    texture.cpp
    types.hpp
    types.cpp
    draw_state.hpp
    draw_state.cpp
    color_transfer.hpp
)

add_subdirectory(transfer)

find_package(glfw3 CONFIG REQUIRED)
find_package(glm REQUIRED)

target_link_libraries(gpu
PRIVATE
    static_files
    glfw
    logger
    stb
PUBLIC
    glad
    glm::glm
)   

add_sanitizers(gpu)

