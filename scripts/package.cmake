cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

# Run after building: cmake [-DBUILD_DIR=build] [-DCONFIG=Release] -P scripts/package.cmake
# This deliberately packages existing outputs without configuring or building them.
get_filename_component(project_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
if(NOT DEFINED BUILD_DIR)
    set(BUILD_DIR "${project_root}/build")
endif()
get_filename_component(BUILD_DIR "${BUILD_DIR}" REALPATH)
set(destination "${project_root}/dist")
cmake_path(IS_PREFIX destination "${BUILD_DIR}" NORMALIZE build_in_dist)
if(build_in_dist)
    message(FATAL_ERROR "The build directory must be outside the generated dist directory")
endif()
if(NOT EXISTS "${BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "No configured build at ${BUILD_DIR}")
endif()
load_cache("${BUILD_DIR}" READ_WITH_PREFIX build_
    CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES CMAKE_CXX_COMPILER
    MIXIMUS_ENABLE_CEF NDI_INCLUDE_DIR NDI_LIBRARY)
if(build_CMAKE_CONFIGURATION_TYPES)
    if(NOT DEFINED CONFIG OR NOT CONFIG IN_LIST build_CMAKE_CONFIGURATION_TYPES)
        message(FATAL_ERROR "Set -DCONFIG to one of: ${build_CMAKE_CONFIGURATION_TYPES}")
    endif()
    set(binary_dir "${BUILD_DIR}/${CONFIG}")
else()
    if(DEFINED CONFIG AND NOT CONFIG STREQUAL build_CMAKE_BUILD_TYPE)
        message(FATAL_ERROR "This build uses ${build_CMAKE_BUILD_TYPE}, not ${CONFIG}")
    endif()
    set(binary_dir "${BUILD_DIR}")
endif()
if(EXISTS "${BUILD_DIR}/static/web_build_failed.txt")
    message(FATAL_ERROR "The web build failed. Rebuild successfully before packaging.")
endif()

if(WIN32)
    set(executable "miximus.exe")
    set(assets "static_files.dll")
    set(library_glob "*.dll")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
    # Script mode has no selected compiler. Reuse the build's inspection tool
    # without requiring a Developer Command Prompt or changing the IDE setup.
    get_filename_component(compiler_dir "${build_CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(dumpbin NAMES dumpbin HINTS "${compiler_dir}")
    if(dumpbin)
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${dumpbin}")
    else()
        find_program(objdump NAMES llvm-objdump objdump REQUIRED)
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${objdump}")
    endif()
    # These are installed prerequisites, even when a copy exists in the build
    # tree or an SDK. Never turn the package into an app-local CRT deployment.
    set(system_runtime_names "^(msvcp[0-9]|msvcr[0-9]|vcruntime[0-9]|concrt[0-9]|vcomp[0-9]|ucrtbase|api-ms-|ext-ms-)")
    set(pre_excludes "${system_runtime_names}" "^(nvcuda|nvcuvid|nvencodeapi64)\\.dll$")
    # Older CMake releases return mixed separators; Windows paths are case-insensitive.
    set(post_excludes "[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\]"
        "[/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\]"
        "[/\\\\][Ww][Ii][Nn][Ss][Xx][Ss][/\\\\]")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(executable miximus)
    set(assets libstatic_files.so)
    set(library_glob "*.so*")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM linux+elf)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
    find_program(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND NAMES objdump REQUIRED)
    # Keep glibc, the loader and installed GPU drivers on the destination host.
    set(pre_excludes "^ld-linux" "^lib(c|m|dl|pthread|rt|resolv|util)\\.so"
        "^lib(cuda|nvidia|nvcuvid|nvidia-encode)[.-]")
    # Distribution-managed libraries remain OS package prerequisites.
    set(post_excludes "^/lib(32|64)?/" "^/usr/lib(32|64)?/")
elseif(APPLE)
    set(executable miximus)
    set(assets libstatic_files.dylib)
    set(library_glob "*.dylib")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM macos+macho)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL otool)
    find_program(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND NAMES otool REQUIRED)
    set(post_excludes "^/System/Library/" "^/usr/lib/")
else()
    message(FATAL_ERROR "Unsupported packaging host: ${CMAKE_HOST_SYSTEM_NAME}")
endif()

# NDI is an application-local redistributable, including when the SDK lives
# under a system library directory on Linux.
set(ndi_runtime_names "^(processing\\.ndi\\.lib\\.(x64|x86|arm64)\\.dll|libndi(\\.[0-9]+)*\\.dylib|libndi\\.so(\\.[0-9]+)*)$")
set(ndi_runtime_paths "(^|/)[Pp]rocessing\\.[Nn][Dd][Ii]\\.[Ll]ib\\.[^.]+\\.dll$"
    "(^|/)libndi\\.so(\\.[0-9]+)*$" "(^|/)libndi(\\.[0-9]+)*\\.dylib$")

foreach(required "${binary_dir}/${executable}" "${binary_dir}/${assets}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}. Build Miximus first.")
    endif()
endforeach()
set(executables "${binary_dir}/${executable}")
set(libraries "${binary_dir}/${assets}")
set(search_dirs "${binary_dir}")
if(build_MIXIMUS_ENABLE_CEF)
    # On Linux the CEF wrapper stages outside the configuration directory.
    set(cef_dir "${BUILD_DIR}/cef")
    if(WIN32)
        set(cef_dir "${binary_dir}/cef")
        set(helper miximus_cef_helper.exe)
        set(cef_library libcef.dll)
    else()
        set(helper miximus_cef_helper)
        set(cef_library libcef.so)
    endif()
    foreach(required "${cef_dir}/.staged" "${cef_dir}/${helper}"
        "${cef_dir}/${cef_library}" "${cef_dir}/icudtl.dat" "${cef_dir}/resources.pak")
        if(NOT EXISTS "${required}")
            message(FATAL_ERROR "Missing CEF output: ${required}. Build Miximus first.")
        endif()
    endforeach()
    list(APPEND executables "${cef_dir}/${helper}")
    # Include dynamically loaded CEF/ANGLE/SwiftShader dependencies too.
    file(GLOB cef_libraries "${cef_dir}/${library_glob}")
    list(APPEND libraries ${cef_libraries})
    list(APPEND search_dirs "${cef_dir}")
endif()

message(STATUS "Resolving runtime dependencies in ${binary_dir}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${executables}
    LIBRARIES ${libraries}
    DIRECTORIES ${search_dirs}
    PRE_EXCLUDE_REGEXES ${pre_excludes}
    POST_INCLUDE_REGEXES ${ndi_runtime_paths}
    POST_EXCLUDE_REGEXES ${post_excludes}
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved)
if(unresolved)
    message(FATAL_ERROR "Unresolved runtime dependencies: ${unresolved}")
endif()

# The build's staged DLL normally has no adjacent notices. Look in the selected
# SDK too, and fail before replacing dist if its required notices are missing.
set(ndi_notice_dirs)
foreach(dependency IN LISTS dependencies)
    get_filename_component(name "${dependency}" NAME)
    string(TOLOWER "${name}" name)
    if(name MATCHES "${ndi_runtime_names}")
        get_filename_component(ndi_directory "${dependency}" DIRECTORY)
        list(APPEND ndi_notice_dirs "${ndi_directory}")
    endif()
endforeach()
if(ndi_notice_dirs)
    get_filename_component(ndi_sdk_root "${build_NDI_INCLUDE_DIR}" DIRECTORY)
    get_filename_component(ndi_library_dir "${build_NDI_LIBRARY}" DIRECTORY)
    get_filename_component(ndi_arch "${ndi_library_dir}" NAME)
    find_file(NDI_LICENSE_FILE NAMES Processing.NDI.Lib.Licenses.txt
        PATHS ${ndi_notice_dirs} "${ndi_library_dir}"
            "${ndi_sdk_root}/Bin/${ndi_arch}" "${ndi_sdk_root}"
        NO_DEFAULT_PATH REQUIRED)
endif()
if(WIN32 AND build_MIXIMUS_ENABLE_CEF)
    # Windows child processes do not search the parent executable's directory.
    # Resolve the helper tree separately so its external DLLs can sit beside it.
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${cef_dir}/${helper}"
        LIBRARIES ${cef_libraries}
        DIRECTORIES ${search_dirs}
        PRE_EXCLUDE_REGEXES ${pre_excludes}
        POST_EXCLUDE_REGEXES ${post_excludes}
        RESOLVED_DEPENDENCIES_VAR cef_dependencies)
endif()

# Fixed, repository-local destinations; never remove arbitrary caller paths.
# Refuse unowned directories rather than deleting user files on the first run.
set(package_state "${project_root}/build-packaging")
set(staging "${package_state}/staging")
cmake_path(IS_PREFIX package_state "${BUILD_DIR}" NORMALIZE build_in_package_state)
if(build_in_package_state)
    message(FATAL_ERROR "build-packaging is reserved for packaging bookkeeping")
endif()
if(IS_SYMLINK "${destination}" OR IS_SYMLINK "${package_state}" OR IS_SYMLINK "${staging}")
    message(FATAL_ERROR "Packaging destinations must not be symbolic links")
endif()
file(MAKE_DIRECTORY "${package_state}")
file(LOCK "${package_state}/package.lock" GUARD PROCESS TIMEOUT 0)
if(EXISTS "${destination}" AND NOT EXISTS "${package_state}/dist-owned"
    AND NOT EXISTS "${destination}/.miximus-package")
    message(FATAL_ERROR "dist already exists and is not owned by this script. Move it aside first.")
endif()
# Accept the previous in-dist marker once; publishing removes legacy bookkeeping.
file(WRITE "${package_state}/dist-owned" "Generated distribution: ${destination}\n")
file(MAKE_DIRECTORY "${destination}")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}")
file(COPY "${binary_dir}/${executable}" "${binary_dir}/${assets}" DESTINATION "${staging}")
if(ndi_notice_dirs)
    configure_file("${NDI_LICENSE_FILE}" "${staging}/Processing.NDI.Lib.Licenses.txt" COPYONLY)
endif()
if(build_MIXIMUS_ENABLE_CEF)
    file(COPY "${cef_dir}/" DESTINATION "${staging}/cef"
        PATTERN ".staged" EXCLUDE PATTERN "*.pdb" EXCLUDE)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        # Only libcef and its resources belong on the main process search path.
        file(COPY "${BUILD_DIR}/cef-link/" DESTINATION "${staging}/cef-link")
    endif()
endif()
foreach(dependency IN LISTS dependencies)
    file(REAL_PATH "${dependency}" real_dependency)
    if(build_MIXIMUS_ENABLE_CEF)
        cmake_path(IS_PREFIX cef_dir "${real_dependency}" NORMALIZE is_cef)
        if(is_cef)
            continue()
        endif()
    endif()
    file(COPY "${dependency}" DESTINATION "${staging}" FOLLOW_SYMLINK_CHAIN)
    if(WIN32 AND dependency IN_LIST cef_dependencies)
        file(COPY "${dependency}" DESTINATION "${staging}/cef")
    endif()
endforeach()

if(WIN32 AND build_MIXIMUS_ENABLE_CEF)
    # CEF is copied as a resource tree; apply the same prerequisite policy to
    # any separate runtime an SDK may have staged there.
    file(GLOB_RECURSE packaged_files LIST_DIRECTORIES FALSE "${staging}/*")
    foreach(packaged_file IN LISTS packaged_files)
        get_filename_component(name "${packaged_file}" NAME)
        string(TOLOWER "${name}" name)
        if(name MATCHES "${system_runtime_names}")
            file(REMOVE "${packaged_file}")
        endif()
    endforeach()
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    # Preserve the build binaries and use a relocatable search path at launch.
    # Never add cef/ here: its Vulkan/ANGLE libraries must stay isolated.
    file(WRITE "${staging}/run-miximus" [=[#!/bin/sh
package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
export LD_LIBRARY_PATH="$package_dir:$package_dir/cef-link${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$package_dir/miximus" "$@"
]=])
    file(CHMOD "${staging}/run-miximus" PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
elseif(APPLE)
    # Rewrite absolute Mach-O install names in the copies, never in the build.
    include(BundleUtilities)
    fixup_bundle("${staging}/${executable}" "${staging}/${assets}" "${staging};${search_dirs}")
endif()

# Publish only after dependency discovery, copying and platform fixups succeed.
file(GLOB old_entries LIST_DIRECTORIES TRUE "${destination}/*")
foreach(entry IN LISTS old_entries)
    file(REMOVE_RECURSE "${entry}")
endforeach()
file(GLOB new_entries LIST_DIRECTORIES TRUE "${staging}/*")
foreach(entry IN LISTS new_entries)
    get_filename_component(name "${entry}" NAME)
    file(RENAME "${entry}" "${destination}/${name}")
endforeach()
file(REMOVE_RECURSE "${staging}")
message(STATUS "Packaged Miximus in ${destination}")
