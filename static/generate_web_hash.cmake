# Script to generate content hash for web files to trigger rebuilds when they change
# This script should be called from CMake

function(generate_web_files_hash source_dir hash_file)
    # Check if source directory exists
    if(NOT EXISTS "${source_dir}")
        message(WARNING "Web source directory does not exist: ${source_dir}")
        file(WRITE "${hash_file}" "SOURCE_DIR_MISSING")
        return()
    endif()
    
    # Get all files in the source directory recursively
    file(GLOB_RECURSE all_files RELATIVE "${source_dir}" "${source_dir}/*")
    
    # Filter out hidden files and directories
    list(FILTER all_files EXCLUDE REGEX "^\\..*")
    
    # Sort files for consistent hashing
    list(SORT all_files)
    
    # Calculate hash of all file contents and modification times
    set(content_hash "")
    foreach(file ${all_files})
        set(full_path "${source_dir}/${file}")
        if(EXISTS "${full_path}" AND NOT IS_DIRECTORY "${full_path}")
            # Get file modification time and size for quick change detection
            file(TIMESTAMP "${full_path}" file_time "%Y%m%d%H%M%S")
            file(SIZE "${full_path}" file_size)
            
            # For smaller files, include content hash; for larger files, use time+size
            if(file_size LESS 10240)  # 10KB threshold
                file(SHA256 "${full_path}" file_hash)
                string(APPEND content_hash "${file}:${file_time}:${file_size}:${file_hash}\n")
            else()
                string(APPEND content_hash "${file}:${file_time}:${file_size}\n")
            endif()
        endif()
    endforeach()
    
    # Hash the combined content
    string(SHA256 combined_hash "${content_hash}")
    
    # Check if hash file exists and has the same content
    set(should_update TRUE)
    if(EXISTS "${hash_file}")
        file(READ "${hash_file}" existing_hash)
        string(STRIP "${existing_hash}" existing_hash)
        if("${existing_hash}" STREQUAL "${combined_hash}")
            set(should_update FALSE)
        endif()
    endif()
    
    # Update hash file if needed
    if(should_update)
        file(WRITE "${hash_file}" "${combined_hash}")
        message(STATUS "Web files changed - hash updated: ${combined_hash}")
        
        # Remove the web_files.cpp to force its regeneration
        if(DEFINED WEB_FILES_CPP AND EXISTS "${WEB_FILES_CPP}")
            file(REMOVE "${WEB_FILES_CPP}")
            message(STATUS "Removed web_files.cpp to trigger rebuild")
        endif()
    else()
        message(STATUS "Web files unchanged")
    endif()
endfunction()

# Main execution
if(DEFINED WEB_SOURCE_DIR AND DEFINED WEB_HASH_FILE)
    generate_web_files_hash("${WEB_SOURCE_DIR}" "${WEB_HASH_FILE}")
endif()
