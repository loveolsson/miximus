add_library(nodes
    node.hpp
    node.cpp
    interface.hpp
    interface.cpp
    interface_type.hpp
    connection.hpp
    connection.cpp
    register_all.hpp
    register_all.cpp
    node_map.hpp    
    validate_option.hpp    
)

add_subdirectory(decklink)
add_subdirectory(math)
add_subdirectory(screen)
add_subdirectory(ffmpeg)
add_subdirectory(utils)
add_subdirectory(teleprompter)
add_subdirectory(debug)
add_subdirectory(composite)

find_package(nlohmann_json REQUIRED)

target_link_libraries(nodes
PRIVATE
    gpu
    decklink
    ndi
    render
    Boost::fiber
    magic_enum::magic_enum
PUBLIC
    nlohmann_json::nlohmann_json
)

add_sanitizers(nodes)
