add_library(nodes
    node.hpp
    node_fwd.hpp
    node_definition.hpp
    node.cpp
    interface.hpp
    interface_fwd.hpp
    interface.cpp
    interface_type.hpp
    connection.hpp
    connection.cpp
    frame_execution.hpp
    frame_execution_fwd.hpp
    frame_execution.cpp
    register_all.hpp
    register_all.cpp
    node_map.hpp
    node_map_fwd.hpp
    option_result.hpp
    normalize_option.hpp
)

add_subdirectory(decklink)
add_subdirectory(ndi)
add_subdirectory(math)
add_subdirectory(screen)
add_subdirectory(ffmpeg)
add_subdirectory(utils)
add_subdirectory(teleprompter)
add_subdirectory(text)
add_subdirectory(debug)
add_subdirectory(generators)
add_subdirectory(composite)
add_subdirectory(switch)
add_subdirectory(system)

find_package(nlohmann_json REQUIRED)

target_link_libraries(nodes
PRIVATE
    gpu
    decklink
    ndi
    render
    media
    miximus_types
    Boost::fiber
    magic_enum::magic_enum
PUBLIC
    nlohmann_json::nlohmann_json
)

if(MIXIMUS_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(nodes PRIVATE
        <boost/asio/executor_work_guard.hpp>
        <boost/asio/io_context.hpp>
        <nlohmann/json.hpp>
    )
endif()

add_sanitizers(nodes)
