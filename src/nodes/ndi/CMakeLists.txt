target_sources(nodes
PRIVATE
    registry_fwd.hpp
    registry.hpp
    registry.cpp
)

target_link_libraries(nodes
PUBLIC
    ndi
)
