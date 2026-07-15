add_library(web_server
    server.hpp
    server_fwd.hpp
    server.cpp
    payload.hpp
    payload.cpp
    templates.hpp
    templates.cpp
    detail/server_impl.cpp
    detail/server_impl.hpp
    detail/headers.cpp
    detail/headers.hpp
    detail/html.hpp
    detail/websocket_connection.hpp
    detail/custom-config.hpp
    detail/custom-logger.hpp
)

find_package(websocketpp REQUIRED)
find_package(nlohmann_json REQUIRED)

target_link_libraries(web_server 
PRIVATE
    static_files
    Boost::headers
    Boost::url
    Threads::Threads
    websocketpp::websocketpp
    logger
PUBLIC
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
) 

if(MIXIMUS_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(web_server PRIVATE <nlohmann/json.hpp>)
endif()

add_sanitizers(web_server)
