miximus_add_node_family(ndi)

target_sources(nodes_ndi
PRIVATE
    registry_fwd.hpp
    registry.hpp
    registry.cpp
    register.hpp
    register.cpp
    detail/input_capture.hpp
    detail/input_capture.cpp
    detail/output_sender.hpp
    detail/output_sender.cpp
    input.cpp
    output.cpp
)

target_link_libraries(nodes_ndi
PUBLIC
    ndi
)

target_link_libraries(nodes_ndi PRIVATE media shutdown_watchdog)
