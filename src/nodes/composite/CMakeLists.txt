miximus_add_node_family(composite)

target_sources(nodes_composite
PRIVATE
    register.hpp
    register.cpp
    draw_box.cpp
    infinite_multiviewer.cpp
    source_labels.cpp
    source_labels.hpp
    mix_tex_2.cpp
)

target_link_libraries(nodes_composite PRIVATE render cpu_task_worker)
