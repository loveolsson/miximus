miximus_add_node_family(system)

target_sources(nodes_system
PRIVATE
    register.hpp
    register.cpp
    settings.cpp
    settings_migrations.hpp
    settings_migrations.cpp
)

if(MIXIMUS_ENABLE_CEF)
    target_link_libraries(nodes_system PRIVATE cef_runtime)
endif()
