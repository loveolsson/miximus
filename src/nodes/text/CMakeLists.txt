miximus_add_node_family(text)

target_sources(nodes_text
PRIVATE
    register.hpp
    register.cpp
    text.cpp
)

target_link_libraries(nodes_text PRIVATE render cpu_task_worker)
