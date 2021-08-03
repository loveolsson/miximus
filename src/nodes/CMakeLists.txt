add_library(nodes
    node.hpp
    node.cpp
    interface.hpp
    interface.cpp
    connection.hpp
    connection.cpp
    register.hpp
    register.cpp
    node_map.hpp    
    validate_option.hpp    
)

add_subdirectory(decklink)
add_subdirectory(math)
add_subdirectory(screen)
add_subdirectory(utils)

target_link_libraries(nodes
PRIVATE
    gpu
    decklink
)