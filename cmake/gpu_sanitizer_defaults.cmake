# Share the same runtime hooks with every executable that links the GPU library,
# including benchmarks and tests. Objects must reach the final executable: sanitizer
# weak hooks alone do not cause an archive member to be extracted by the linker.
include_guard(GLOBAL)
if(NOT (SANITIZE_ADDRESS OR SANITIZE_UNDEFINED))
    return()
endif()

add_library(gpu_sanitizer_defaults OBJECT ${PROJECT_SOURCE_DIR}/src/sanitizer_defaults.cpp)
set_target_properties(gpu_sanitizer_defaults PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(gpu_sanitizer_defaults PRIVATE
    $<$<BOOL:${SANITIZE_ADDRESS}>:MIXIMUS_SANITIZE_ADDRESS>
    $<$<BOOL:${SANITIZE_UNDEFINED}>:MIXIMUS_SANITIZE_UNDEFINED>
)
if(SANITIZE_ADDRESS AND TARGET cuda_dependencies)
    target_compile_definitions(gpu_sanitizer_defaults PRIVATE MIXIMUS_CUDA_ASAN_COMPAT)
endif()
if(SANITIZE_ADDRESS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(MIXIMUS_SANITIZER_ADDR2LINE_EXECUTABLE NAMES addr2line REQUIRED)
    target_compile_definitions(gpu_sanitizer_defaults PRIVATE
        MIXIMUS_SANITIZER_ADDR2LINE="${MIXIMUS_SANITIZER_ADDR2LINE_EXECUTABLE}"
    )
endif()

add_library(gpu_sanitizer_runtime INTERFACE)
target_link_libraries(gpu_sanitizer_runtime INTERFACE $<TARGET_OBJECTS:gpu_sanitizer_defaults>)
target_link_libraries(gpu PRIVATE gpu_sanitizer_runtime)
