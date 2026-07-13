add_library(app
    app_state.hpp
    app_state_fwd.hpp
    app_state.cpp
    configuration.hpp
    configuration_fwd.hpp
    configuration.cpp
    node_manager.hpp
    node_manager_fwd.hpp
    node_manager.cpp
    node_status_registry.hpp
    node_status_registry_fwd.hpp
    node_status_registry.cpp
)

add_subdirectory(adapters)

target_link_libraries(app 
PRIVATE
    gpu
    render
    nodes
    magic_enum::magic_enum
    Boost::system
)

#add_sanitizers(app)
