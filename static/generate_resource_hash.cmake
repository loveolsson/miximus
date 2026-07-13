# Generate a hash of the resource directory and invalidate the generated bundle
# when any resource changes.

function(generate_resource_files_hash source_dir hash_file)
    if(NOT EXISTS "${source_dir}")
        message(WARNING "Resource source directory does not exist: ${source_dir}")
        file(WRITE "${hash_file}" "SOURCE_DIR_MISSING")
        return()
    endif()

    file(GLOB_RECURSE all_files RELATIVE "${source_dir}" "${source_dir}/*")
    list(FILTER all_files EXCLUDE REGEX "^\\..*")
    list(SORT all_files)

    set(content_hash "")
    foreach(file ${all_files})
        set(full_path "${source_dir}/${file}")
        if(EXISTS "${full_path}" AND NOT IS_DIRECTORY "${full_path}")
            file(TIMESTAMP "${full_path}" file_time "%Y%m%d%H%M%S")
            file(SIZE "${full_path}" file_size)

            if(file_size LESS 10240)
                file(SHA256 "${full_path}" file_hash)
                string(APPEND content_hash "${file}:${file_time}:${file_size}:${file_hash}\n")
            else()
                string(APPEND content_hash "${file}:${file_time}:${file_size}\n")
            endif()
        endif()
    endforeach()

    string(SHA256 combined_hash "${content_hash}")

    set(should_update TRUE)
    if(EXISTS "${hash_file}")
        file(READ "${hash_file}" existing_hash)
        string(STRIP "${existing_hash}" existing_hash)
        if("${existing_hash}" STREQUAL "${combined_hash}")
            set(should_update FALSE)
        endif()
    endif()

    if(should_update)
        file(WRITE "${hash_file}" "${combined_hash}")
        message(STATUS "Resource files changed - hash updated: ${combined_hash}")

        if(DEFINED RESOURCE_FILES_CPP AND EXISTS "${RESOURCE_FILES_CPP}")
            file(REMOVE "${RESOURCE_FILES_CPP}")
            message(STATUS "Removed resource_files.cpp to trigger rebuild")
        endif()
    else()
        message(STATUS "Resource files unchanged")
    endif()
endfunction()

if(DEFINED RESOURCE_SOURCE_DIR AND DEFINED RESOURCE_HASH_FILE)
    generate_resource_files_hash("${RESOURCE_SOURCE_DIR}" "${RESOURCE_HASH_FILE}")
endif()
