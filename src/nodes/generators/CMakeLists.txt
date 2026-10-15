miximus_add_node_family(generators)

target_sources(nodes_generators
PRIVATE
    register.hpp
    register.cpp
    test_pattern.cpp
    image.cpp
)

target_link_libraries(nodes_generators PRIVATE render cpu_task_worker)
