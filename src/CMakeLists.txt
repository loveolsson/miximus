file (GLOB_RECURSE SOURCES 
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
)

add_executable(${ProjectName}
    ${SOURCES}
)

target_link_libraries(${ProjectName}
    nlohmann_json::nlohmann_json
    glfw
    glad
    static_files
    fmt
) 

target_include_directories(${ProjectName} PRIVATE
    ${CMAKE_SOURCE_DIR}/submodules/websocketpp
    ${CMAKE_SOURCE_DIR}/submodules/asio/include
)

target_compile_definitions(${ProjectName} PRIVATE ASIO_STANDALONE)