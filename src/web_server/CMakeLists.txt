add_library(web_server
    include/web_server/web_server.hpp
    src/web_server.cpp
    src/web_server_impl.cpp
    src/web_server_impl.hpp
    src/websocket_connection.hpp
)

target_link_libraries(web_server 
PRIVATE
    static_files
PUBLIC    
    messages
) 

target_include_directories(web_server 
PRIVATE
    ${CMAKE_SOURCE_DIR}/submodules/websocketpp
    ${CMAKE_SOURCE_DIR}/submodules/asio/asio/include
PUBLIC
    include
)