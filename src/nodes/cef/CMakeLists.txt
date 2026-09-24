target_sources(nodes PRIVATE register.hpp register.cpp subsystem_fwd.hpp browser_options.hpp)
if(MIXIMUS_ENABLE_CEF)
    target_sources(nodes PRIVATE browser.cpp)
else()
    target_sources(nodes PRIVATE browser_unavailable.cpp)
endif()
target_compile_definitions(nodes PUBLIC MIXIMUS_ENABLE_CEF=$<BOOL:${MIXIMUS_ENABLE_CEF}>)
add_library(cef_media_input STATIC detail/media_input_pool.hpp detail/media_input_pool.cpp)
target_include_directories(cef_media_input PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cef_media_input PUBLIC Threads::Threads)
add_sanitizers(cef_media_input)

add_library(cef_ingress STATIC
    detail/frame_pool.hpp
    detail/frame_pool.cpp
)
target_link_libraries(cef_ingress PUBLIC gpu)
add_sanitizers(cef_ingress)

if(BUILD_TESTING)
    add_executable(cef_media_input_test tests/media_input_pool_test.cpp)
    target_link_libraries(cef_media_input_test PRIVATE cef_media_input GTest::gtest_main)
    add_sanitizers(cef_media_input_test)
    gtest_discover_tests(cef_media_input_test)
    add_executable(cef_ingress_vulkan_test tests/frame_pool_test.cpp)
    target_link_libraries(cef_ingress_vulkan_test PRIVATE cef_ingress logger GTest::gtest_main)
    add_sanitizers(cef_ingress_vulkan_test)
    if(MIXIMUS_TEST_VULKAN)
        gtest_discover_tests(cef_ingress_vulkan_test PROPERTIES LABELS "vulkan;hardware")
    endif()
endif()

if(MIXIMUS_ENABLE_CEF)
    target_sources(miximus_cef_helper PRIVATE detail/renderer_app.hpp detail/renderer_app.cpp detail/command_protocol.hpp detail/command_messages.hpp)
    add_library(cef_runtime STATIC detail/runtime.hpp detail/runtime.cpp)
    target_sources(cef_runtime PRIVATE
        session.hpp
        detail/task.hpp
        detail/capture_timing.hpp
        detail/browser_session.hpp detail/browser_session.cpp
        detail/command_channel.hpp detail/command_channel.cpp
        detail/capture_stream.hpp detail/capture_stream.cpp
    )
    target_sources(cef_runtime PRIVATE subsystem.hpp subsystem.cpp)
    target_link_libraries(cef_runtime PUBLIC cef_ingress media)
    target_link_libraries(cef_runtime PRIVATE cef_sdk Threads::Threads ${CMAKE_DL_LIBS})
    target_include_directories(cef_runtime PUBLIC ${PROJECT_SOURCE_DIR}/src)
    add_dependencies(cef_runtime miximus_cef_helper)
    add_sanitizers(cef_runtime)
    target_link_libraries(nodes PRIVATE cef_runtime)
    if(BUILD_TESTING)
        add_executable(cef_subsystem_probe tests/subsystem_probe.cpp)
        target_sources(cef_subsystem_probe PRIVATE ${PROJECT_SOURCE_DIR}/src/gpu/tests/color_compare.cpp)
        target_link_libraries(cef_subsystem_probe PRIVATE vulkan_dependencies magic_enum::magic_enum)
        miximus_compile_shader(cef_subsystem_probe ${PROJECT_SOURCE_DIR}/src/gpu/tests/color_compare.comp)
        target_compile_definitions(cef_subsystem_probe PRIVATE
            MIXIMUS_CEF_COMPARE_SHADER="${CMAKE_CURRENT_BINARY_DIR}/shaders/color_compare.comp.spv")
        target_link_libraries(cef_subsystem_probe PRIVATE cef_runtime cef_sdk gpu logger)
        set_target_properties(cef_subsystem_probe PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
        add_sanitizers(cef_subsystem_probe)
        add_executable(cef_runtime_probe tests/runtime_probe.cpp)
        target_link_libraries(cef_runtime_probe PRIVATE cef_runtime gpu logger ${CMAKE_DL_LIBS})
        set_target_properties(cef_runtime_probe PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
        add_sanitizers(cef_runtime_probe)
        add_executable(cef_accelerated_probe tests/accelerated_probe.cpp)
        target_link_libraries(cef_accelerated_probe PRIVATE cef_runtime cef_sdk gpu logger)
        set_target_properties(cef_accelerated_probe PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
        add_sanitizers(cef_accelerated_probe)
        add_executable(cef_session_probe tests/session_probe.cpp)
        target_link_libraries(cef_session_probe PRIVATE cef_runtime gpu logger)
        set_target_properties(cef_session_probe PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
        add_sanitizers(cef_session_probe)
    endif()
endif()
