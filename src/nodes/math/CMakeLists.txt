miximus_add_node_family(math)

target_sources(nodes_math
PRIVATE
    register.hpp
    register.cpp
    math.cpp
    lerp.cpp
    easing.cpp
    clamp.cpp
)
