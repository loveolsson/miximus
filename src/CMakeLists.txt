include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

add_subdirectory(logger)
add_subdirectory(types)
add_subdirectory(utils)
add_subdirectory(web_server)
add_subdirectory(messages)
add_subdirectory(nodes)
add_subdirectory(gpu)
add_subdirectory(wrapper)
add_subdirectory(application)

set(CMAKE_CXX_CLANG_TIDY ${MIXIMUS_CLANG_TIDY})

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
    logger

    ffmpeg
    decklink
    cef
    video-sdk
    Boost::system

    ${CMAKE_DL_LIBS}
)

target_include_directories(miximus PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
)

macro(copy_library_to_binary_dir NAME)
add_custom_command(TARGET miximus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:${NAME}> $<TARGET_FILE_DIR:miximus>
)
endmacro()

copy_library_to_binary_dir(static_files)
copy_library_to_binary_dir(msquic)