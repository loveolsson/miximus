# GPU-only ingress building blocks. No CEF runtime or browser node is registered yet.
add_library(cef_ingress STATIC
    detail/frame_pool.hpp
    detail/frame_pool.cpp
)
target_link_libraries(cef_ingress PUBLIC gpu)
add_sanitizers(cef_ingress)

if(BUILD_TESTING)
    add_executable(cef_ingress_vulkan_test tests/frame_pool_test.cpp)
    target_link_libraries(cef_ingress_vulkan_test PRIVATE cef_ingress logger GTest::gtest_main)
    add_sanitizers(cef_ingress_vulkan_test)
    if(MIXIMUS_TEST_VULKAN)
        gtest_discover_tests(cef_ingress_vulkan_test PROPERTIES LABELS "vulkan;hardware")
    endif()
endif()
