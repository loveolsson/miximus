miximus_add_node_family(screen)

target_sources(nodes_screen
PRIVATE
    register.hpp
    register.cpp
    detail/output_presenter.hpp
    detail/output_presenter.cpp
    screen_output.cpp
    screen_output_migrations.hpp
    screen_output_migrations.cpp
)

target_link_libraries(nodes_screen PRIVATE media)
