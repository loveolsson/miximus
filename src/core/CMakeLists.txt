add_library(app
    app_state.hpp
    app_state_fwd.hpp
    app_state.cpp
    clock_source.hpp
    clock_source.cpp
    command_line_options.hpp
    command_line_options.cpp
    configuration.hpp
    configuration_fwd.hpp
    configuration.cpp
    frame_context.hpp
    frame_scheduler.hpp
    frame_scheduler_fwd.hpp
    frame_scheduler.cpp
    node_manager.hpp
    node_manager_fwd.hpp
    node_manager.cpp
    origin_info.hpp
    node_status_registry.hpp
    node_status_registry_fwd.hpp
    node_status_registry.cpp
    test_instrumentation/render_thread_delay.hpp
    test_instrumentation/render_thread_delay.cpp
)

add_subdirectory(adapters)

target_link_libraries(app 
PRIVATE
    gpu
    render
    nodes
    miximus_types
    magic_enum::magic_enum
    Boost::headers
    Boost::program_options
)

if(MIXIMUS_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(app PRIVATE
        <boost/asio/executor_work_guard.hpp>
        <boost/asio/io_context.hpp>
        <nlohmann/json.hpp>
    )
endif()

add_sanitizers(app)

if(BUILD_TESTING)
    add_executable(core_test
        tests/settings_node_test.cpp
        tests/frame_execution_test.cpp
        tests/frame_scheduler_test.cpp
        tests/output_timing_test.cpp
        tests/source_timing_test.cpp
        tests/command_line_options_test.cpp
        tests/node_status_registry_test.cpp
        tests/web_message_test.cpp
    )
    target_link_libraries(
        core_test
        PRIVATE app magic_enum::magic_enum GTest::gtest_main
    )
    if(MIXIMUS_ENABLE_CLANG_TIDY)
        set_property(
            TARGET core_test
            PROPERTY CXX_CLANG_TIDY
                     "${MIXIMUS_CLANG_TIDY_EXECUTABLE};--checks=-readability-function-cognitive-complexity"
        )
    endif()
    add_sanitizers(core_test)
    gtest_discover_tests(core_test)
endif()
