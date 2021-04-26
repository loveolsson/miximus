include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(web_server)

add_executable(${ProjectName}
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

target_link_libraries(${ProjectName}
    glfw
    glad
    static_files
    msquic
    web_server
) 

#target_include_directories(${ProjectName} PRIVATE
#    ${CMAKE_SOURCE_DIR}/submodules/websocketpp
#    ${CMAKE_SOURCE_DIR}/submodules/asio/asio/include
#    ${CMAKE_SOURCE_DIR}/submodules/json/include
#)