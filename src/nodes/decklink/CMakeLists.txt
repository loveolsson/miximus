target_sources(nodes
PRIVATE
    detail/allocator.hpp
    detail/allocator.cpp
    decklink.hpp
    input.cpp
    output.cpp
    registry_fwd.hpp
    registry.hpp
    registry.cpp
)

target_link_libraries(nodes
PUBLIC
    decklink
)