option(MIXIMUS_ENABLE_CUDA "Build optional CUDA/Vulkan transfers when the toolkit is available" ON)
if(NOT MIXIMUS_ENABLE_CUDA)
    return()
endif()
block(SCOPE_FOR VARIABLES PROPAGATE CUDAToolkit_FOUND CUDAToolkit_VERSION CUDAToolkit_BIN_DIR)
    # FindCUDAToolkit probes optional libraries beyond the cudart we consume.
    # Do not search executable PATH directories for each missing library. Keep
    # PATH for programs (including nvcc), LIB for libraries and INCLUDE for headers.
    # Explicit toolkit roots, CMake search paths and platform defaults still apply.
    if((NOT DEFINED CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OR CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH)
        AND (NOT DEFINED CMAKE_FIND_USE_CMAKE_PATH OR CMAKE_FIND_USE_CMAKE_PATH))
        cmake_path(CONVERT "$ENV{PATH}" TO_CMAKE_PATH_LIST cuda_program_paths)
        cmake_path(CONVERT "$ENV{LIB}" TO_CMAKE_PATH_LIST cuda_library_paths)
        cmake_path(CONVERT "$ENV{INCLUDE}" TO_CMAKE_PATH_LIST cuda_include_paths)
        list(APPEND CMAKE_PROGRAM_PATH ${cuda_program_paths})
        list(APPEND CMAKE_LIBRARY_PATH ${cuda_library_paths})
        list(APPEND CMAKE_INCLUDE_PATH ${cuda_include_paths})
        set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
    endif()
    find_package(CUDAToolkit QUIET)
endblock()
if(CUDAToolkit_FOUND AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
    add_library(cuda_dependencies INTERFACE)
    target_link_libraries(cuda_dependencies INTERFACE CUDA::cudart)
    if(WIN32)
        # FindCUDAToolkit exposes cudart as UNKNOWN IMPORTED, so CMake cannot
        # include its runtime in TARGET_RUNTIME_DLLS. Describe the matching DLL
        # separately, retaining the toolkit target's usage requirements.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(CUDA_RUNTIME_ARCH arm64)
        else()
            set(CUDA_RUNTIME_ARCH x64)
        endif()
        file(GLOB CUDA_RUNTIME_DLLS
            "${CUDAToolkit_BIN_DIR}/${CUDA_RUNTIME_ARCH}/cudart64_*.dll"
            "${CUDAToolkit_BIN_DIR}/cudart64_*.dll"
        )
        list(LENGTH CUDA_RUNTIME_DLLS CUDA_RUNTIME_DLL_COUNT)
        if(NOT CUDA_RUNTIME_DLL_COUNT EQUAL 1)
            message(FATAL_ERROR "Expected one CUDA runtime DLL in ${CUDAToolkit_BIN_DIR} for ${CUDA_RUNTIME_ARCH}; found: ${CUDA_RUNTIME_DLLS}")
        endif()
        get_target_property(CUDA_RUNTIME_IMPLIB CUDA::cudart IMPORTED_LOCATION)
        add_library(cuda_runtime SHARED IMPORTED GLOBAL)
        set_target_properties(cuda_runtime PROPERTIES
            IMPORTED_IMPLIB "${CUDA_RUNTIME_IMPLIB}"
            IMPORTED_LOCATION "${CUDA_RUNTIME_DLLS}"
        )
        target_link_libraries(cuda_dependencies INTERFACE cuda_runtime)
    endif()
    message(STATUS "CUDA/Vulkan transfers enabled (${CUDAToolkit_VERSION}); opt in with --use-cuda after startup qualification")
elseif(CUDAToolkit_FOUND)
    message(STATUS "Found CUDA Toolkit ${CUDAToolkit_VERSION}; CUDA/Vulkan transfers are not yet implemented for ${CMAKE_SYSTEM_NAME}, using Vulkan staging")
else()
    message(STATUS "CUDA Toolkit not found; using Vulkan staging")
endif()
