miximus_add_node_family(decklink)

target_sources(nodes_decklink
PRIVATE
    register.hpp
    register.cpp
    detail/allocator.hpp
    detail/allocator.cpp
    detail/device_monitor.hpp
    detail/device_monitor.cpp
    detail/device_reservation.hpp
    detail/input_capture.hpp
    detail/input_capture.cpp
    detail/output_video_buffer.hpp
    detail/output_activation.hpp
    detail/output_activation.cpp
    detail/output_path.hpp
    detail/output_path.cpp
    input.cpp
    output.cpp
    output_migrations.hpp
    output_migrations.cpp
    registry_fwd.hpp
    registry.hpp
    registry.cpp
)

target_link_libraries(nodes_decklink
PRIVATE
    gpu
PUBLIC
    decklink
)

target_link_libraries(nodes_decklink PRIVATE media shutdown_watchdog)
