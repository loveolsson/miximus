add_library(render)

target_sources(render PRIVATE
    detail/color_lut.hpp
    detail/rec709_quantizer.hpp
    pill_label.cpp
    pill_label.hpp
)

add_subdirectory(font)
add_subdirectory(image_asset)
add_subdirectory(surface)
add_subdirectory(test_pattern)

target_link_libraries(render
PRIVATE
    logger
    gpu
    miximus_types
    static_files
    stb
)

add_sanitizers(render)

if(BUILD_TESTING)
    add_executable(render_test
        tests/surface_test.cpp
        tests/pill_label_test.cpp
        tests/font_instance_test.cpp
    )
    target_link_libraries(render_test PRIVATE render logger GTest::gtest_main)
    add_sanitizers(render_test)
    miximus_stage_runtime_dlls(render_test)
    gtest_discover_tests(render_test)
endif()
