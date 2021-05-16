include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

add_subdirectory(utils)
add_subdirectory(web_server)
add_subdirectory(messages)
add_subdirectory(nodes)
add_subdirectory(gpu)

add_executable(miximus
    ${MIXIMUS_MAIN_SOURCES}
)

target_link_libraries(miximus
    glfw
    glad
    glm::glm
    msquic
    web_server
    static_files

    libcef_lib
    libcef_dll_wrapper
    ${CMAKE_DL_LIBS}
)

add_custom_command(TARGET miximus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:static_files> $<TARGET_FILE_DIR:miximus>
)

add_custom_command(TARGET miximus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:miximus>
)