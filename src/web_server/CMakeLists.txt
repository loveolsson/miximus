set(CMAKE_CXX_CLANG_TIDY ${MIXIMUS_CLANG_TIDY})

add_library(web_server
    server.hpp
    server.cpp
    server_impl.cpp
    server_impl.hpp
    websocket_connection.hpp
    custom-config.hpp
    custom-logger.hpp
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