target_sources(nodes PRIVATE
    register.hpp
    register.cpp
    registry_fwd.hpp
    registry.hpp
    registry.cpp
    detail/decoder.hpp
    detail/decoder.cpp
    detail/hardware_devices.hpp
    detail/hardware_devices.cpp
    detail/playback.hpp
    detail/playback.cpp
    detail/playback_timeline.hpp
    detail/texture_pool.hpp
    playback_looping.cpp
)
target_link_libraries(nodes PRIVATE ffmpeg_platform)

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(nodes PRIVATE
        detail/vulkan_ingest.cpp
        detail/vulkan_ingest.hpp
        detail/hardware_device.cpp
        detail/hardware_device.hpp
    )
    target_link_libraries(nodes PRIVATE vulkan_dependencies)
else()
    target_sources(nodes PRIVATE
        detail/vulkan_ingest_stub.cpp
        detail/vulkan_ingest.hpp
        detail/hardware_device_stub.cpp
        detail/hardware_device.hpp
    )
endif()

if(BUILD_TESTING)
    add_executable(ffmpeg_playback_test
        tests/decoder_test.cpp
        tests/playback_timeline_test.cpp
    )
    target_link_libraries(ffmpeg_playback_test PRIVATE nodes ffmpeg GTest::gtest_main)
    add_sanitizers(ffmpeg_playback_test)
    miximus_stage_runtime_dlls(ffmpeg_playback_test)
    gtest_discover_tests(ffmpeg_playback_test)

    # Hardware validation is run explicitly, outside ordinary CTest.
    add_executable(ffmpeg_gpu_test tests/texture_pool_test.cpp)
    target_link_libraries(ffmpeg_gpu_test PRIVATE nodes gpu logger GTest::gtest_main)
    if(WIN32 AND NOT MIXIMUS_ENABLE_CEF)
        target_sources(ffmpeg_gpu_test PRIVATE tests/playback_no_cef_test.cpp)
        target_link_libraries(ffmpeg_gpu_test PRIVATE app)
    endif()
    add_sanitizers(ffmpeg_gpu_test)
    miximus_stage_runtime_dlls(ffmpeg_gpu_test)
endif()

if(WIN32)
    target_sources(nodes PRIVATE detail/platform_ingest_win.cpp detail/platform_ingest.hpp)
    target_link_libraries(nodes PRIVATE d3d11_dependencies)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(nodes PRIVATE detail/platform_ingest_linux.cpp detail/platform_ingest.hpp)
else()
    target_sources(nodes PRIVATE detail/platform_ingest_stub.cpp detail/platform_ingest.hpp)
endif()
