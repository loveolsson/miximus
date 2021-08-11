add_library(ndi INTERFACE)

target_include_directories(ndi
INTERFACE
    ${CMAKE_SOURCE_DIR}/3rd-party/ndi-sdk/include
)

if(WIN32) 



else()

target_link_libraries(ndi
INTERFACE
    ${CMAKE_SOURCE_DIR}/3rd-party/ndi-sdk/lib/x86_64-linux-gnu/libndi.so
)

endif()