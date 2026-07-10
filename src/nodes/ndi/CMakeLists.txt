target_sources(nodes
PRIVATE
    registry_fwd.hpp
    registry.hpp
    registry.cpp
    register.hpp
    register.cpp
    input.cpp
    output.cpp
)

target_link_libraries(nodes
PUBLIC
    ndi
)
