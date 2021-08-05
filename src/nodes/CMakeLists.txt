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
add_subdirectory(teleprompter)
add_subdirectory(debug)

find_package(nlohmann_json REQUIRED)

target_link_libraries(nodes
PRIVATE
    gpu
    decklink
    render
PUBLIC
    nlohmann_json::nlohmann_json
)