add_library(gpu
    vertex_buffer.hpp
    vertex_buffer.cpp
    vertex_array.hpp
    vertex_array.cpp
    context.hpp
    context_fwd.hpp
    context.cpp
    context_logging.hpp
    context_logging.cpp
    shader.hpp
    shader.cpp
    framebuffer.hpp
    framebuffer_fwd.hpp
    framebuffer.cpp
    vertex.hpp
    vertex_attr.hpp
    sync.hpp
    sync.cpp
    texture.hpp
    texture_fwd.hpp
    texture.cpp
    types.hpp
    types.cpp
    geometry.hpp
    draw_state.hpp
    draw_state.cpp
    textured_quad.hpp
    textured_quad.cpp
    color_transfer.hpp
    detail/monitor_platform.hpp
)

if(WIN32)
    target_sources(gpu PRIVATE detail/monitor_platform_win.cpp)
elseif(APPLE)
    target_sources(gpu PRIVATE detail/monitor_platform_mac.cpp)
    find_library(COLORSYNC_FRAMEWORK ColorSync REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(gpu PRIVATE ${COLORSYNC_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK} ${COREGRAPHICS_FRAMEWORK})
else()
    target_sources(gpu PRIVATE detail/monitor_platform_linux.cpp)
    find_package(X11 REQUIRED COMPONENTS Xrandr)
    target_link_libraries(gpu PRIVATE X11::Xrandr)
endif()

add_subdirectory(transfer)

find_package(glfw3 CONFIG REQUIRED)
find_package(glm REQUIRED)

target_link_libraries(gpu
PRIVATE
    static_files
    glfw
    logger
    stb
    miximus_types
    $<$<PLATFORM_ID:Linux>:GL>   # provides glXGetCurrentContext for DVP EGL detection
PUBLIC
    glad_gl
    glm::glm
    cuda
    dvp
)

if(MIXIMUS_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(gpu PRIVATE <spdlog/spdlog.h>)
endif()

add_sanitizers(gpu)
