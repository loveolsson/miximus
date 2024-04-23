add_library(web_server
    server.hpp
    server.cpp
    payload.hpp
    templates.hpp
    detail/server_impl.cpp
    detail/server_impl.hpp
    detail/headers.cpp
    detail/headers.hpp
    detail/websocket_connection.hpp
    detail/custom-config.hpp
    detail/custom-logger.hpp
)

find_package(websocketpp REQUIRED)
find_package(nlohmann_json REQUIRED)

target_link_libraries(web_server 
PRIVATE
    static_files
    Boost::system
    Threads::Threads
    websocketpp::websocketpp
    logger
PUBLIC
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
) 

add_sanitizers(web_server)