# Script to hash web source files and project config files.
# If anything changed since the last build, runs `npm run build` in WEB_DIR.
# Expected variables (passed via -D):
#   WEB_DIR               - path to the web/ project directory
#   SRC_HASH_FILE         - persistent hash file (in CMake binary dir)
#   NPM_LS_STAMP_FILE     - touched after a successful `npm ls` run
#   WEB_BUILD_FAILED_FILE - marker file written on failure, removed on success

function(compute_web_src_hash web_dir out_hash)
    set(src_dir "${web_dir}/src")

    if(NOT EXISTS "${src_dir}")
        set(${out_hash} "SOURCE_DIR_MISSING" PARENT_SCOPE)
        return()
    endif()

    file(GLOB_RECURSE src_files RELATIVE "${src_dir}" "${src_dir}/*")
    list(FILTER src_files EXCLUDE REGEX "^\\..*")
    list(SORT src_files)

    set(content "")
    foreach(rel_file ${src_files})
        set(full_path "${src_dir}/${rel_file}")
        if(EXISTS "${full_path}" AND NOT IS_DIRECTORY "${full_path}")
            file(TIMESTAMP "${full_path}" mtime "%Y%m%d%H%M%S")
            file(SIZE "${full_path}" fsize)
            if(fsize LESS 102400)
                file(SHA256 "${full_path}" fhash)
                string(APPEND content "src/${rel_file}:${mtime}:${fsize}:${fhash}\n")
            else()
                string(APPEND content "src/${rel_file}:${mtime}:${fsize}\n")
            endif()
        endif()
    endforeach()

    # Include top-level project files that affect the build output
    foreach(config_file
        "${web_dir}/package.json"
        "${web_dir}/tsconfig.json"
        "${web_dir}/vite.config.ts"
        "${web_dir}/index.html"
    )
        if(EXISTS "${config_file}")
            file(TIMESTAMP "${config_file}" mtime "%Y%m%d%H%M%S")
            file(SIZE "${config_file}" fsize)
            file(SHA256 "${config_file}" fhash)
            get_filename_component(fname "${config_file}" NAME)
            string(APPEND content "${fname}:${mtime}:${fsize}:${fhash}\n")
        endif()
    endforeach()

    string(SHA256 combined "${content}")
    set(${out_hash} "${combined}" PARENT_SCOPE)
endfunction()

# ---- Helpers -------------------------------------------------------------

macro(web_fail msg)
    message(WARNING "${msg}")
    file(WRITE "${WEB_BUILD_FAILED_FILE}" "${msg}")
    return()
endmacro()

# ---- Main ----------------------------------------------------------------
# Expected variables (passed via -D):
#   WEB_DIR               - path to the web/ project directory
#   SRC_HASH_FILE         - persistent hash file (in CMake binary dir)
#   NPM_LS_STAMP_FILE     - touched after a successful `npm ls` run
#   WEB_BUILD_FAILED_FILE - marker file written on failure, removed on success

if(NOT DEFINED WEB_DIR OR NOT DEFINED SRC_HASH_FILE
   OR NOT DEFINED NPM_LS_STAMP_FILE OR NOT DEFINED WEB_BUILD_FAILED_FILE)
    message(FATAL_ERROR "check_web_src.cmake requires WEB_DIR, SRC_HASH_FILE, NPM_LS_STAMP_FILE, and WEB_BUILD_FAILED_FILE")
endif()

# 1. Check npm is available ------------------------------------------------
find_program(NPM_EXECUTABLE npm)
if(NOT NPM_EXECUTABLE)
    web_fail("npm not found on PATH. Install Node.js (https://nodejs.org) to build the Web UI.")
endif()

# 2. Validate dependencies with `npm ls` -----------------------------------
# Only runs when package.json or package-lock.json changed since the last
# successful check.  No network access; reads only local node_modules.
if(NOT EXISTS "${WEB_DIR}/node_modules")
    web_fail("Web UI dependencies are not installed. Run 'npm install' in the web/ directory.")
endif()

set(_needs_ls FALSE)
if(NOT EXISTS "${NPM_LS_STAMP_FILE}")
    set(_needs_ls TRUE)
else()
    file(TIMESTAMP "${NPM_LS_STAMP_FILE}" _stamp_mtime "%Y%m%d%H%M%S")
    foreach(_watch "${WEB_DIR}/package.json" "${WEB_DIR}/package-lock.json")
        if(EXISTS "${_watch}")
            file(TIMESTAMP "${_watch}" _f_mtime "%Y%m%d%H%M%S")
            if(_f_mtime GREATER _stamp_mtime)
                set(_needs_ls TRUE)
                break()
            endif()
        endif()
    endforeach()
endif()

if(_needs_ls)
    message(STATUS "Checking Web UI dependencies (npm ls)...")
    execute_process(
        COMMAND "${NPM_EXECUTABLE}" ls --depth=0
        WORKING_DIRECTORY "${WEB_DIR}"
        RESULT_VARIABLE _ls_result
        OUTPUT_VARIABLE _ls_output
        ERROR_VARIABLE  _ls_error
    )
    if(NOT _ls_result EQUAL 0)
        web_fail("Web UI dependencies are missing or invalid. Run 'npm install' in the web/ directory.")
    endif()
    file(TOUCH "${NPM_LS_STAMP_FILE}")
    message(STATUS "Web UI dependencies OK")
else()
    message(STATUS "Web UI dependencies unchanged - skipping check")
endif()

# 3. Check source hash; run npm build only when something changed ----------
compute_web_src_hash("${WEB_DIR}" new_hash)

set(needs_build TRUE)
if(EXISTS "${SRC_HASH_FILE}")
    file(READ "${SRC_HASH_FILE}" stored_hash)
    string(STRIP "${stored_hash}" stored_hash)
    if("${stored_hash}" STREQUAL "${new_hash}")
        set(needs_build FALSE)
    endif()
endif()

if(needs_build)
    message(STATUS "Web source changed - running npm run build...")
    execute_process(
        COMMAND "${NPM_EXECUTABLE}" run build
        WORKING_DIRECTORY "${WEB_DIR}"
        RESULT_VARIABLE npm_result
        COMMAND_ECHO STDOUT
    )
    if(NOT npm_result EQUAL 0)
        web_fail("npm run build failed with exit code ${npm_result}")
    endif()
    file(WRITE "${SRC_HASH_FILE}" "${new_hash}")
    if(EXISTS "${WEB_BUILD_FAILED_FILE}")
        file(REMOVE "${WEB_BUILD_FAILED_FILE}")
    endif()
    message(STATUS "Web build complete - hash updated")
else()
    # Source unchanged and all pre-checks passed - clear any stale failure marker
    if(EXISTS "${WEB_BUILD_FAILED_FILE}")
        file(REMOVE "${WEB_BUILD_FAILED_FILE}")
    endif()
    message(STATUS "Web source unchanged - skipping npm build")
endif()
