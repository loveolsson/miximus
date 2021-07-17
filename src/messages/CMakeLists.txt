add_library(messages INTERFACE
    payload.hpp
    templates.hpp
)

target_link_libraries(messages INTERFACE nlohmann_json::nlohmann_json)