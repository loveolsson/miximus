add_library(node_framework STATIC
    action.hpp
    action.cpp
    node.hpp
    node_fwd.hpp
    node_definition.hpp
    node.cpp
    interface.hpp
    interface_fwd.hpp
    interface.cpp
    interface_type.hpp
    source_info.hpp
    disconnected_value_provider.hpp
    disconnected_value_provider.cpp
    frame_execution.hpp
    frame_execution_fwd.hpp
    frame_execution.cpp
    node_map.hpp
    node_map_fwd.hpp
    option_result.hpp
    normalize_option.hpp
)

find_package(nlohmann_json REQUIRED)
target_link_libraries(node_framework PUBLIC gpu miximus_types nlohmann_json::nlohmann_json
    PRIVATE logger magic_enum::magic_enum)
target_compile_definitions(node_framework PUBLIC MIXIMUS_ENABLE_CEF=$<BOOL:${MIXIMUS_ENABLE_CEF}>)
add_sanitizers(node_framework)

# Registration is the composition layer; implementations do not depend on it.
add_library(nodes STATIC register_all.hpp register_all.cpp)
target_link_libraries(nodes PUBLIC node_framework)
add_sanitizers(nodes)

function(miximus_add_node_family family)
    add_library(nodes_${family} STATIC)
    target_link_libraries(nodes_${family} PUBLIC node_framework PRIVATE logger magic_enum::magic_enum)
    target_link_libraries(nodes PRIVATE nodes_${family})
    add_sanitizers(nodes_${family})
    if(MIXIMUS_USE_PRECOMPILED_HEADERS)
        target_precompile_headers(nodes_${family} PRIVATE <nlohmann/json.hpp>)
    endif()
endfunction()

add_subdirectory(decklink)
add_subdirectory(ndi)
add_subdirectory(ffmpeg)
add_subdirectory(cef)
add_subdirectory(math)
add_subdirectory(screen)
add_subdirectory(utils)
add_subdirectory(teleprompter)
add_subdirectory(text)
add_subdirectory(debug)
add_subdirectory(generators)
add_subdirectory(composite)
add_subdirectory(switch)
add_subdirectory(system)
