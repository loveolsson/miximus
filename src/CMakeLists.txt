if(WIN32) 
# Set project wide definitions to make Windows includes behave
# add_compile_definitions(_WIN32_WINNT=0x0A00)
#add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(logger)
add_subdirectory(types)
add_subdirectory(utils)
add_subdirectory(web_server)
add_subdirectory(media)
add_subdirectory(nodes)
add_subdirectory(gpu)
add_subdirectory(wrapper)
add_subdirectory(core)
add_subdirectory(render)


add_executable(miximus
    main.cpp
)

if(SANITIZE_ADDRESS OR SANITIZE_UNDEFINED)
    target_sources(miximus PRIVATE sanitizer_defaults.cpp)

    target_compile_definitions(miximus PRIVATE
        $<$<BOOL:${SANITIZE_ADDRESS}>:MIXIMUS_SANITIZE_ADDRESS>
        $<$<BOOL:${SANITIZE_UNDEFINED}>:MIXIMUS_SANITIZE_UNDEFINED>
    )

    if(SANITIZE_ADDRESS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        find_program(MIXIMUS_SANITIZER_ADDR2LINE_EXECUTABLE NAMES addr2line)
        if(NOT MIXIMUS_SANITIZER_ADDR2LINE_EXECUTABLE)
            message(FATAL_ERROR "addr2line is required for Clang sanitizer builds")
        endif()

        target_compile_definitions(miximus PRIVATE
            MIXIMUS_SANITIZER_ADDR2LINE="${MIXIMUS_SANITIZER_ADDR2LINE_EXECUTABLE}"
        )
    endif()
endif()

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

macro(copy_library_to_binary_dir NAME)
add_custom_command(TARGET miximus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:${NAME}> $<TARGET_FILE_DIR:miximus>
)
endmacro()

copy_library_to_binary_dir(static_files)

if(WIN32)
    foreach(BOOST_RUNTIME url fiber context filesystem atomic program_options)
        if(TARGET Boost::${BOOST_RUNTIME})
            copy_library_to_binary_dir(Boost::${BOOST_RUNTIME})
        endif()
    endforeach()
endif()
