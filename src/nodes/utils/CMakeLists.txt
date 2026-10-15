miximus_add_node_family(utils)

target_sources(nodes_utils
PRIVATE
    register.hpp
    register.cpp
    rect.cpp
    vec2.cpp
    set_source_name.cpp
    framebuffer.cpp
    framebuffer_to_texture.cpp
)
