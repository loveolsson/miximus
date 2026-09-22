option(MIXIMUS_ENABLE_CEF "Build the accelerated CEF browser subsystem" OFF)
set(MIXIMUS_CEF_ROOT "" CACHE PATH "Extracted pinned CEF standard SDK")
if(NOT MIXIMUS_ENABLE_CEF)
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "CEF currently has a pinned Linux x64 SDK only; other platforms require their own qualification")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sdk.json" CEF_MANIFEST)
string(JSON MIXIMUS_CEF_VERSION GET "${CEF_MANIFEST}" version)
string(JSON api_version GET "${CEF_MANIFEST}" api_version)
set(CEF_ROOT "${MIXIMUS_CEF_ROOT}")
if(NOT EXISTS "${CEF_ROOT}/include/cef_version.h")
    message(FATAL_ERROR "Set MIXIMUS_CEF_ROOT to the SDK pinned in src/wrapper/cef/sdk.json")
endif()
file(STRINGS "${CEF_ROOT}/include/cef_version.h" CEF_VERSION_LINE REGEX "^#define CEF_VERSION ")
if(NOT CEF_VERSION_LINE STREQUAL "#define CEF_VERSION \"${MIXIMUS_CEF_VERSION}\"")
    message(FATAL_ERROR "CEF SDK does not match the pinned version: ${CEF_VERSION_LINE}")
endif()
# Scope all upstream settings to this directory and its wrapper child.
set(USE_SANDBOX ON)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
# Upstream wrapper sources use upstream warning settings, not Miximus' strict
# first-party warning set. Restore the latter for our helper below.
get_directory_property(MIXIMUS_CEF_PARENT_OPTIONS COMPILE_OPTIONS)
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "${MIXIMUS_CEF_PARENT_OPTIONS}")

set(CEF_STAGE "${CMAKE_BINARY_DIR}/cef")
file(MAKE_DIRECTORY "${CEF_STAGE}")
set(CEF_STAGE_COMMANDS)
set(CEF_STAGE_DEPENDENCIES
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk.json"
    "${CEF_ROOT}/include/cef_version.h"
)
foreach(FILE IN LISTS CEF_BINARY_FILES)
    if(NOT EXISTS "${CEF_ROOT}/Release/${FILE}")
        message(FATAL_ERROR "Missing CEF runtime file: ${FILE}")
    endif()
    list(APPEND CEF_STAGE_COMMANDS COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CEF_ROOT}/Release/${FILE}" "${CEF_STAGE}/${FILE}")
    list(APPEND CEF_STAGE_DEPENDENCIES "${CEF_ROOT}/Release/${FILE}")
endforeach()
foreach(FILE IN LISTS CEF_RESOURCE_FILES)
    if(NOT EXISTS "${CEF_RESOURCE_DIR}/${FILE}")
        message(FATAL_ERROR "Missing CEF resource: ${FILE}")
    endif()
    if(IS_DIRECTORY "${CEF_RESOURCE_DIR}/${FILE}")
        file(GLOB_RECURSE CEF_RESOURCE_DEPENDENCIES CONFIGURE_DEPENDS "${CEF_RESOURCE_DIR}/${FILE}/*")
        list(APPEND CEF_STAGE_DEPENDENCIES ${CEF_RESOURCE_DEPENDENCIES})
        list(APPEND CEF_STAGE_COMMANDS COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CEF_RESOURCE_DIR}/${FILE}" "${CEF_STAGE}/${FILE}")
    else()
        list(APPEND CEF_STAGE_DEPENDENCIES "${CEF_RESOURCE_DIR}/${FILE}")
        list(APPEND CEF_STAGE_COMMANDS COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CEF_RESOURCE_DIR}/${FILE}" "${CEF_STAGE}/${FILE}")
    endif()
endforeach()
foreach(FILE LICENSE.txt CREDITS.html)
    list(APPEND CEF_STAGE_COMMANDS COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CEF_ROOT}/${FILE}" "${CEF_STAGE}/${FILE}")
    list(APPEND CEF_STAGE_DEPENDENCIES "${CEF_ROOT}/${FILE}")
endforeach()
# Patched builds retain the upstream version string. Track the binary itself,
# and stage its provenance alongside it, rather than relying on the header date.
file(GLOB CEF_SOURCE_MANIFEST CONFIGURE_DEPENDS "${CEF_ROOT}/miximus-source-build.json")
if(CEF_SOURCE_MANIFEST)
    list(APPEND CEF_STAGE_DEPENDENCIES "${CEF_ROOT}/miximus-source-build.json")
    list(APPEND CEF_STAGE_COMMANDS COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CEF_ROOT}/miximus-source-build.json" "${CEF_STAGE}/miximus-source-build.json")
else()
    list(APPEND CEF_STAGE_COMMANDS COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${CEF_STAGE}/miximus-source-build.json")
endif()
add_custom_command(OUTPUT "${CEF_STAGE}/.staged"
    ${CEF_STAGE_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${CEF_STAGE}/.staged"
    DEPENDS ${CEF_STAGE_DEPENDENCIES}
    VERBATIM
)
add_custom_target(cef_runtime_files DEPENDS "${CEF_STAGE}/.staged")
# libcef must be a direct executable dependency (its close interposer uses
# RTLD_NEXT). Expose only libcef to that executable's RUNPATH, never the bundled
# Vulkan/ANGLE libraries. Chromium children still run from the complete cef/ tree.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cef-link")
# CEF resolves ICU/snapshot assets relative to the dladdr path of libcef even
# when resources_dir_path is set. Expose data links here, but no other libraries.
foreach(FILE libcef.so v8_context_snapshot.bin ${CEF_RESOURCE_FILES})
    file(CREATE_LINK "../cef/${FILE}" "${CMAKE_BINARY_DIR}/cef-link/${FILE}" SYMBOLIC)
    install(FILES "${CMAKE_BINARY_DIR}/cef-link/${FILE}" DESTINATION cef-link)
endforeach()
add_library(cef_binary SHARED IMPORTED GLOBAL)
set_target_properties(cef_binary PROPERTIES IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/cef-link/libcef.so")
add_dependencies(cef_binary cef_runtime_files)
add_library(cef_sdk INTERFACE)
target_include_directories(cef_sdk SYSTEM INTERFACE "${CEF_ROOT}")
target_compile_definitions(cef_sdk INTERFACE CEF_API_VERSION=${api_version})
target_link_libraries(cef_sdk INTERFACE libcef_dll_wrapper cef_binary ${CEF_STANDARD_LIBS})

add_executable(miximus_cef_helper helper.cpp)
target_link_libraries(miximus_cef_helper PRIVATE cef_sdk)
set_target_properties(miximus_cef_helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CEF_STAGE}"
    BUILD_RPATH "$ORIGIN"
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN"
)
install(TARGETS miximus_cef_helper RUNTIME DESTINATION cef)
install(DIRECTORY "${CEF_STAGE}/" DESTINATION cef USE_SOURCE_PERMISSIONS
    PATTERN ".staged" EXCLUDE PATTERN "miximus_cef_helper" EXCLUDE)
