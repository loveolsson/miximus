find_package(Vulkan 1.3 REQUIRED GLOBAL COMPONENTS glslangValidator)
set(MIXIMUS_GLSLANG_VERSION "16.2.0" CACHE STRING "Pinned glslang compiler version")
execute_process(
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" --version
    OUTPUT_VARIABLE MIXIMUS_GLSLANG_REPORTED
    COMMAND_ERROR_IS_FATAL ANY
)
string(FIND "${MIXIMUS_GLSLANG_REPORTED}" "glslang Khronos. ${MIXIMUS_GLSLANG_VERSION}" MIXIMUS_GLSLANG_MATCH)
if(MIXIMUS_GLSLANG_MATCH LESS 0)
    message(FATAL_ERROR "Expected glslang ${MIXIMUS_GLSLANG_VERSION}. Select a different toolchain explicitly with MIXIMUS_GLSLANG_VERSION.")
endif()

find_program(MIXIMUS_SPIRV_VAL NAMES spirv-val REQUIRED)
find_package(glfw3 3.4 CONFIG REQUIRED GLOBAL)

# Upstream code is pinned in Git submodules, never copied into src/.
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/submodules/volk ${CMAKE_CURRENT_BINARY_DIR}/volk EXCLUDE_FROM_ALL)
add_subdirectory(${PROJECT_SOURCE_DIR}/submodules/vma ${CMAKE_CURRENT_BINARY_DIR}/vma EXCLUDE_FROM_ALL)
set_target_properties(volk VulkanMemoryAllocator PROPERTIES SYSTEM TRUE)
target_link_libraries(volk PUBLIC Vulkan::Headers)

add_library(vulkan_dependencies STATIC allocator.cpp)
target_link_libraries(vulkan_dependencies PUBLIC volk::volk GPUOpen::VulkanMemoryAllocator)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(MIXIMUS_DRM_INCLUDE_DIR drm_fourcc.h PATH_SUFFIXES libdrm drm REQUIRED)
    target_include_directories(vulkan_dependencies SYSTEM PUBLIC "${MIXIMUS_DRM_INCLUDE_DIR}")
endif()
target_compile_definitions(vulkan_dependencies PUBLIC
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)

# The implementation is upstream code instantiated by our wrapper.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vulkan_dependencies PRIVATE -w)
endif()

function(miximus_compile_shader target source)
    get_filename_component(shader_name "${source}" NAME)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.spv")

    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND Vulkan::glslangValidator
            --target-env vulkan1.3
            -V "${source}"
            "-I${PROJECT_SOURCE_DIR}/shaders"
            -o "${output}"
        COMMAND "${MIXIMUS_SPIRV_VAL}" --target-env vulkan1.3 "${output}"
        DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/shaders/common.glsl"
        VERBATIM
    )

    target_sources(${target} PRIVATE "${output}")
    set_property(TARGET ${target} APPEND PROPERTY MIXIMUS_SPIRV_FILES "${output}")
endfunction()

if(APPLE)
    # Match the installed SDK's MoltenVK package; do not copy its headers into src/.
    set(MIXIMUS_MOLTENVK_VERSION "1.4.1" CACHE STRING "Pinned MoltenVK package version")
    find_path(MIXIMUS_MOLTENVK_INCLUDE_DIR MoltenVK/mvk_private_api.h
        HINTS "$ENV{VULKAN_SDK}/include" "${Vulkan_INCLUDE_DIRS}"
        REQUIRED
    )
    file(READ "${MIXIMUS_MOLTENVK_INCLUDE_DIR}/MoltenVK/mvk_private_api.h" MIXIMUS_MOLTENVK_HEADER)
    foreach(part MAJOR MINOR PATCH)
        string(REGEX MATCH "#define[ \t]+MVK_VERSION_${part}[ \t]+([0-9]+)" match "${MIXIMUS_MOLTENVK_HEADER}")
        set(MIXIMUS_MOLTENVK_${part} "${CMAKE_MATCH_1}")
    endforeach()
    set(MIXIMUS_MOLTENVK_FOUND_VERSION "${MIXIMUS_MOLTENVK_MAJOR}.${MIXIMUS_MOLTENVK_MINOR}.${MIXIMUS_MOLTENVK_PATCH}")
    if(NOT MIXIMUS_MOLTENVK_FOUND_VERSION VERSION_EQUAL MIXIMUS_MOLTENVK_VERSION)
        message(FATAL_ERROR "Expected MoltenVK ${MIXIMUS_MOLTENVK_VERSION}, found ${MIXIMUS_MOLTENVK_FOUND_VERSION}")
    endif()
    message(STATUS "MoltenVK ${MIXIMUS_MOLTENVK_FOUND_VERSION}; runtime loaded through the Vulkan SDK ICD")
endif()
