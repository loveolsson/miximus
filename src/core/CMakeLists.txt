add_library(app
    app_state.hpp
    app_state.cpp
    node_manager.hpp
    node_manager.cpp
)

add_subdirectory(adapters)

target_link_libraries(app 
PRIVATE
    gpu
    render
    nodes
)