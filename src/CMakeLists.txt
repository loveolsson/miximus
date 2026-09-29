if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(wrapper)
add_subdirectory(logger)
add_subdirectory(types)
add_subdirectory(utils)
add_subdirectory(web_server)
add_subdirectory(media)
add_subdirectory(nodes)
add_subdirectory(gpu)
add_subdirectory(core)
add_subdirectory(render)


add_executable(miximus
    main.cpp
)

target_link_libraries(miximus
PRIVATE
    web_server
    logger
    render
    app
    gpu
    shutdown_watchdog
    magic_enum::magic_enum
)

add_sanitizers(miximus)
miximus_stage_runtime_dlls(miximus)

macro(copy_library_to_binary_dir NAME)
add_custom_command(TARGET miximus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:${NAME}> $<TARGET_FILE_DIR:miximus>
)
endmacro()

if(NOT WIN32)
    copy_library_to_binary_dir(static_files)
endif()
