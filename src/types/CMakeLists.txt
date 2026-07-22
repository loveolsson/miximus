add_library(miximus_types
    action.hpp
    error.hpp
    frame_rate.hpp
    frame_rate.cpp
    topic.hpp
    settings_option.hpp
    settings_option.cpp
)

find_package(nlohmann_json REQUIRED)
target_link_libraries(miximus_types PUBLIC nlohmann_json::nlohmann_json)

add_sanitizers(miximus_types)
