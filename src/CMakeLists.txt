include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(web_server)
add_subdirectory(config)
add_subdirectory(messages)

add_executable(${ProjectName}
    main.cpp
)

target_link_libraries(${ProjectName}
    glfw
    glad
    msquic
    web_server
    config
) 

add_custom_command(TARGET ${ProjectName} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:static_files> $<TARGET_FILE_DIR:${ProjectName}>
)

add_custom_command(TARGET ${ProjectName} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:${ProjectName}>
)