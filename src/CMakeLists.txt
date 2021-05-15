include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(MIXIMUS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

add_subdirectory(utils)
add_subdirectory(web_server)
add_subdirectory(messages)
add_subdirectory(nodes)

add_executable(${ProjectName}
    ${MIXIMUS_SOURCES}
)

target_link_libraries(${ProjectName}
    glfw
    glad
    glm::glm
    msquic
    web_server
) 

add_custom_command(TARGET ${ProjectName} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:static_files> $<TARGET_FILE_DIR:${ProjectName}>
)

add_custom_command(TARGET ${ProjectName} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:${ProjectName}>
)