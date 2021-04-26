project(web_server)

add_library(web_server
    ${CMAKE_CURRENT_SOURCE_DIR}/web_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/web_server.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/detail/web_server_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/detail/web_server_impl.hpp
)

target_link_libraries(web_server
    static_files
) 

target_compile_definitions(web_server PRIVATE 
    ASIO_STANDALONE
    _WEBSOCKETPP_CPP11_INTERNAL_
    ASIO_HAS_STD_TYPE_TRAITS
    ASIO_HAS_STD_SHARED_PTR 
    ASIO_HAS_STD_ADDRESSOF
    ASIO_HAS_STD_ATOMIC
    ASIO_HAS_STD_CHRONO 
    ASIO_HAS_CSTDINT
    ASIO_HAS_STD_ARRAY 
    ASIO_HAS_STD_SYSTEM_ERROR
)



target_include_directories(web_server PRIVATE
    ${CMAKE_SOURCE_DIR}/submodules/websocketpp
    ${CMAKE_SOURCE_DIR}/submodules/asio/asio/include
)

target_include_directories(web_server PUBLIC
    ${CMAKE_SOURCE_DIR}/submodules/json/include
    ${CMAKE_SOURCE_DIR}/submodules/concurrentqueue
)