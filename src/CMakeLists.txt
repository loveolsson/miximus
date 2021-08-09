if(WIN32) 
# Set project wide definitions to make Windows includes behave
# add_compile_definitions(_WIN32_WINNT=0x0A00)
#add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_CXX_CLANG_TIDY ${MIXIMUS_CLANG_TIDY})

add_subdirectory(logger)
add_subdirectory(types)
add_subdirectory(utils)
add_subdirectory(web_server)
add_subdirectory(nodes)
add_subdirectory(gpu)
add_subdirectory(wrapper)
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

#    msquic
    Boost::system
    Boost::fiber
)

add_sanitizers(miximus)

macro(copy_library_to_binary_dir NAME)
add_custom_command(TARGET miximus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:${NAME}> $<TARGET_FILE_DIR:miximus>
)
endmacro()

copy_library_to_binary_dir(static_files)
copy_library_to_binary_dir(decklink)
copy_library_to_binary_dir(stb)
#copy_library_to_binary_dir(msquic)