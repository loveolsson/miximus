add_library(miximus_types
    action.hpp
    connection.hpp
    error.hpp
    frame_rate.hpp
    frame_rate.cpp
    json_contract_descriptions.hpp
    json_contract.hpp
    node_status.hpp
    node_status_json.hpp
    output_buffer_limits.hpp
    topic.hpp
    settings_option.hpp
    settings_option.cpp
    web_message.hpp
    web_message_json.hpp
    web_message_request.hpp
)

find_package(nlohmann_json REQUIRED)
target_link_libraries(
    miximus_types
    PUBLIC Boost::headers magic_enum::magic_enum nlohmann_json::nlohmann_json
)

add_sanitizers(miximus_types)

if(NOT CMAKE_CROSSCOMPILING)
    find_package(glm REQUIRED)
    add_executable(miximus_typescript_generator typescript_generator.cpp)
    target_link_libraries(
        miximus_typescript_generator
        PRIVATE miximus_types Boost::headers glm::glm magic_enum::magic_enum
    )

    set(MIXIMUS_GENERATED_TYPESCRIPT
        "${CMAKE_SOURCE_DIR}/web/src/generated/json_contracts.ts"
        CACHE INTERNAL "Generated Miximus TypeScript contracts"
    )
    add_custom_target(
        generate_typescript_contracts ALL
        COMMAND $<TARGET_FILE:miximus_typescript_generator> "${MIXIMUS_GENERATED_TYPESCRIPT}"
        DEPENDS miximus_typescript_generator
        COMMENT "Generating TypeScript contracts from C++"
        VERBATIM
    )
endif()
