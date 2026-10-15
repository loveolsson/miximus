add_library(frame_timing STATIC clock_source.hpp clock_source.cpp frame_scheduler.hpp frame_scheduler.cpp)
target_link_libraries(frame_timing PUBLIC miximus_types PRIVATE Threads::Threads)
add_sanitizers(frame_timing)

add_library(node_status STATIC node_status_registry.hpp node_status_registry_fwd.hpp node_status_registry.cpp)
target_link_libraries(node_status PUBLIC miximus_types Boost::headers PRIVATE logger Threads::Threads)
add_sanitizers(node_status)

add_library(app
    app_state.hpp
    app_state_fwd.hpp
    app_state.cpp
    command_line_options.hpp
    command_line_options.cpp
    configuration.hpp
    configuration_fwd.hpp
    configuration.cpp
    frame_context.hpp
    node_actions.hpp
    node_actions.cpp
    node_manager.hpp
    node_manager_fwd.hpp
    node_manager.cpp
    node_file_dialog.cpp
    origin_info.hpp
    test_instrumentation/render_thread_delay.hpp
    test_instrumentation/render_thread_delay.cpp
)

add_subdirectory(adapters)

target_link_libraries(app 
PRIVATE
    environment
    gpu
    render
    nodes
    node_framework
    frame_timing
    node_status
    nodes_decklink
    nodes_ndi
    ffmpeg_playback
    shutdown_watchdog
    magic_enum::magic_enum
    Boost::program_options
PUBLIC
    cpu_task_worker
    file_dialog
    miximus_types
    Boost::headers
    logger
)

if(MIXIMUS_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(app PRIVATE
        <boost/asio/executor_work_guard.hpp>
        <boost/asio/io_context.hpp>
        <nlohmann/json.hpp>
    )
endif()

target_compile_definitions(app PRIVATE MIXIMUS_ENABLE_CEF=$<BOOL:${MIXIMUS_ENABLE_CEF}>)
add_sanitizers(app)

if(MIXIMUS_ENABLE_CEF)
    target_link_libraries(app PRIVATE cef_runtime)
endif()

if(BUILD_TESTING)
    add_executable(core_test
        tests/settings_node_test.cpp
        tests/node_options_test.cpp
        tests/command_line_options_test.cpp
        tests/node_actions_test.cpp
    )
    target_link_libraries(
        core_test
        PRIVATE app magic_enum::magic_enum GTest::gtest_main
    )
    add_sanitizers(core_test)
    miximus_stage_runtime_dlls(core_test)
    gtest_discover_tests(core_test)
endif()

if(BUILD_TESTING)
    add_executable(frame_policy_test tests/frame_scheduler_test.cpp tests/output_timing_test.cpp tests/source_timing_test.cpp)
    target_link_libraries(frame_policy_test PRIVATE frame_timing media GTest::gtest_main)

    add_executable(node_status_test tests/node_status_registry_test.cpp)
    target_link_libraries(node_status_test PRIVATE node_status GTest::gtest_main)

    add_executable(web_message_test tests/web_message_test.cpp)
    target_link_libraries(web_message_test PRIVATE web_server miximus_types GTest::gtest_main)

    add_executable(frame_execution_test tests/frame_execution_test.cpp)
    target_link_libraries(frame_execution_test PRIVATE node_framework nodes_composite nodes_switch nodes_utils gpu GTest::gtest_main)

    foreach(test_target frame_policy_test node_status_test web_message_test frame_execution_test)
        add_sanitizers(${test_target})
        miximus_stage_runtime_dlls(${test_target})
        gtest_discover_tests(${test_target})
    endforeach()
endif()
