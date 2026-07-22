add_library(app
    application_settings.hpp
    application_settings_fwd.hpp
    application_settings.cpp
    app_state.hpp
    app_state_fwd.hpp
    app_state.cpp
    configuration.hpp
    configuration_fwd.hpp
    configuration.cpp
    frame_context.hpp
    node_manager.hpp
    node_manager_fwd.hpp
    node_manager.cpp
    node_status_registry.hpp
    node_status_registry_fwd.hpp
    node_status_registry.cpp
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
    add_executable(application_settings_test
        application_settings_test.cpp
        application_settings.cpp
    )
    target_link_libraries(application_settings_test PRIVATE miximus_types magic_enum::magic_enum)
    add_sanitizers(application_settings_test)
    add_test(NAME application_settings COMMAND application_settings_test)
endif()
