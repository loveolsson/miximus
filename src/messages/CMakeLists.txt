add_library(messages INTERFACE)

target_include_directories(messages INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(messages INTERFACE nlohmann_json::nlohmann_json)