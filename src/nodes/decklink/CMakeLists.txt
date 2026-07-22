target_sources(nodes
PRIVATE
    register.hpp
    register.cpp
    detail/allocator.hpp
    detail/allocator.cpp
    detail/device_monitor.hpp
    detail/device_monitor.cpp
    detail/device_reservation.hpp
    detail/output_video_buffer.hpp
    detail/platform_compat.hpp
    input.cpp
    output.cpp
    registry_fwd.hpp
    registry.hpp
    registry.cpp
)

if(WIN32)
    target_sources(nodes PRIVATE detail/platform_compat_win.cpp)
elseif(APPLE)
    target_sources(nodes PRIVATE detail/platform_compat_mac.cpp)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(nodes PRIVATE ${COREFOUNDATION_FRAMEWORK})
else()
    target_sources(nodes PRIVATE detail/platform_compat_linux.cpp)
endif()

target_link_libraries(nodes
PRIVATE
    gpu
PUBLIC
    decklink
)
