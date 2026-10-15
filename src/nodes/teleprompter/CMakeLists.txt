miximus_add_node_family(teleprompter)

target_sources(nodes_teleprompter
PRIVATE
    register.hpp
    register.cpp
    teleprompter.cpp
)
target_link_libraries(nodes_teleprompter PRIVATE render cpu_task_worker)
