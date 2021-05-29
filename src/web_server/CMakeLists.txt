add_library(web_server
    include/web_server/web_server.hpp
    src/web_server.cpp
    src/web_server_impl.cpp
    src/web_server_impl.hpp
    src/websocket_connection.hpp
    src/custom-config.hpp
    src/custom-logger.hpp
)

target_link_libraries(web_server 
PRIVATE
    static_files
    Boost::system
    Threads::Threads
    websocketpp::websocketpp
    logger
PUBLIC    
    messages
) 

target_include_directories(web_server 
PUBLIC
    include
)