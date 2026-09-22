find_package(nlohmann_json REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm REQUIRED)

add_library(gpu
    device.hpp
    device.cpp
    resource_types.hpp
    texture.hpp
    texture.cpp
    buffer.hpp
    buffer.cpp
    completion.hpp
    completion.cpp
    recording.hpp
    recording.cpp
    presenter.hpp
    presenter.cpp
    detail/device.hpp
    detail/external_image_support.hpp
    detail/external_image_support.cpp
    detail/submission.cpp
    detail/drawing.cpp
    detail/pipeline.cpp
    detail/conversion.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(gpu PRIVATE
        detail/dma_buf_image.hpp detail/dma_buf_image.cpp
        detail/dma_buf_copy.hpp detail/dma_buf_copy.cpp
    )
endif()
target_link_libraries(gpu PUBLIC glm::glm)
target_include_directories(gpu PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gpu PRIVATE
    static_files
    vulkan_dependencies
    magic_enum::magic_enum
    logger
    glfw
    nlohmann_json::nlohmann_json
    Threads::Threads
)

include(${PROJECT_SOURCE_DIR}/cmake/gpu_sanitizer_defaults.cmake)

add_sanitizers(gpu)

if(BUILD_TESTING)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(gpu_dma_buf_vulkan_test tests/dma_buf_image_test.cpp)
        target_link_libraries(gpu_dma_buf_vulkan_test PRIVATE gpu vulkan_dependencies magic_enum::magic_enum logger GTest::gtest_main)
        add_sanitizers(gpu_dma_buf_vulkan_test)
    endif()
    add_executable(gpu_vulkan_test tests/device_test.cpp tests/external_image_device_test.cpp)
    target_link_libraries(gpu_vulkan_test PRIVATE gpu logger GTest::gtest_main nlohmann_json::nlohmann_json)
    add_sanitizers(gpu_vulkan_test)
    # Hardware tests are explicit: ordinary CTest remains usable without a GPU.
    option(MIXIMUS_TEST_VULKAN "Register Vulkan hardware tests with CTest" OFF)
    if(MIXIMUS_TEST_VULKAN)
        if(TARGET gpu_dma_buf_vulkan_test)
            gtest_discover_tests(gpu_dma_buf_vulkan_test PROPERTIES LABELS "vulkan;hardware")
        endif()
        gtest_discover_tests(gpu_vulkan_test PROPERTIES LABELS "vulkan;hardware")
    endif()
endif()

target_sources(gpu PRIVATE
    window.hpp
    window_fwd.hpp
    window.cpp
    texture_fwd.hpp
    texture_frame.hpp
    texture_frame.cpp

    types.hpp
    types.cpp
    geometry.hpp
    drawing.hpp
    color_transfer.hpp
    detail/monitor_platform.hpp
)

if(WIN32)
    target_sources(gpu PRIVATE detail/monitor_platform_win.cpp)
elseif(APPLE)
    target_sources(gpu PRIVATE detail/monitor_platform_mac.cpp)
    find_library(COLORSYNC_FRAMEWORK ColorSync REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(gpu PRIVATE ${COLORSYNC_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK} ${COREGRAPHICS_FRAMEWORK})
else()
    target_sources(gpu PRIVATE detail/monitor_platform_linux.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(gpu PRIVATE x11_dependencies)
    endif()
endif()

add_subdirectory(transfer)

# Build the cross-machine transfer comparison tool only when explicitly requested.
add_executable(miximus_transfer_benchmark EXCLUDE_FROM_ALL benchmarks/transfer_benchmark.cpp)
target_link_libraries(miximus_transfer_benchmark PRIVATE gpu logger nlohmann_json::nlohmann_json)
add_sanitizers(miximus_transfer_benchmark)
set_target_properties(miximus_transfer_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

target_link_libraries(gpu
PRIVATE
    stb
    miximus_types
)

if(MIXIMUS_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(gpu PRIVATE <spdlog/spdlog.h>)
endif()

if(BUILD_TESTING)
    add_executable(gpu_test
        tests/geometry_test.cpp
        tests/texture_test.cpp
        tests/transfer_layout_test.cpp
        tests/external_image_support_test.cpp
    )
    target_link_libraries(gpu_test PRIVATE gpu GTest::gtest_main)
    add_sanitizers(gpu_test)
    gtest_discover_tests(gpu_test)
endif()

if(BUILD_TESTING)
    add_executable(gpu_transfer_vulkan_test tests/transfer_vulkan_test.cpp)
    target_link_libraries(gpu_transfer_vulkan_test PRIVATE gpu logger GTest::gtest_main)
    if(TARGET decklink)
        target_sources(gpu_transfer_vulkan_test PRIVATE ${PROJECT_SOURCE_DIR}/src/nodes/decklink/detail/allocator.cpp)
        target_link_libraries(gpu_transfer_vulkan_test PRIVATE decklink)
        target_compile_definitions(gpu_transfer_vulkan_test PRIVATE MIXIMUS_TEST_DECKLINK=1)
    endif()
    add_sanitizers(gpu_transfer_vulkan_test)
    if(MIXIMUS_TEST_VULKAN)
        gtest_discover_tests(gpu_transfer_vulkan_test PROPERTIES LABELS "vulkan;hardware")
    endif()
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(gpu_window_test tests/window_test.cpp)
    target_link_libraries(gpu_window_test PRIVATE gpu logger glfw GTest::gtest_main)
    add_sanitizers(gpu_window_test)
    if(MIXIMUS_TEST_VULKAN)
        gtest_discover_tests(gpu_window_test PROPERTIES LABELS "window;hardware")
    endif()
endif()
