cmake_minimum_required(VERSION 3.28)
set(CMAKE_CXX_CLANG_TIDY ${MIXIMUS_CLANG_TIDY})

add_subdirectory(bundler)

set(BUNDLER_NAMESPACE "miximus::static_files")

# Generate hash file to track web directory changes  
set(WEB_HASH_FILE "${CMAKE_CURRENT_BINARY_DIR}/web_files.hash")

# Step 1: hash web/src + config files; run `npm run build` only when something changed
set(WEB_SRC_HASH_FILE    "${CMAKE_CURRENT_BINARY_DIR}/web_src.hash")
set(WEB_BUILD_FAILED_FILE "${CMAKE_CURRENT_BINARY_DIR}/web_build_failed.txt")
set(NPM_LS_STAMP_FILE    "${CMAKE_CURRENT_BINARY_DIR}/npm_ls.stamp")

add_custom_target(build_web_if_changed ALL
  COMMAND ${CMAKE_COMMAND}
    -DWEB_DIR=${CMAKE_SOURCE_DIR}/web
    -DSRC_HASH_FILE=${WEB_SRC_HASH_FILE}
    -DNPM_LS_STAMP_FILE=${NPM_LS_STAMP_FILE}
    -DWEB_BUILD_FAILED_FILE=${WEB_BUILD_FAILED_FILE}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_web_src.cmake"
  COMMENT "Checking web source files for changes..."
  VERBATIM
)

# Step 2: hash web/dist; remove web_files.cpp if the bundled output needs refreshing
add_custom_target(check_web_files_hash ALL
  COMMAND ${CMAKE_COMMAND} 
    -DWEB_SOURCE_DIR=${CMAKE_SOURCE_DIR}/web/dist
    -DWEB_HASH_FILE=${WEB_HASH_FILE}
    -DWEB_FILES_CPP=${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp
    -DWEB_BUILD_FAILED_FILE=${WEB_BUILD_FAILED_FILE}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_web_hash.cmake"
  COMMENT "Checking for web file changes..."
  VERBATIM
)

add_dependencies(check_web_files_hash build_web_if_changed)

add_custom_command(
  OUTPUT 
    ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp
  COMMAND 
    $<TARGET_FILE:bundler> 
  ARGS 
    --src ${CMAKE_SOURCE_DIR}/web/dist/ --dst ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp --namespace ${BUNDLER_NAMESPACE} --mapname get_web_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS bundler check_web_files_hash
  COMMENT "Generating web_files.cpp from web/dist/"
)

add_custom_command(
  OUTPUT 
    ${CMAKE_CURRENT_BINARY_DIR}/resource_files.cpp
  COMMAND 
    $<TARGET_FILE:bundler> 
  ARGS 
    --src ${CMAKE_SOURCE_DIR}/resources/ --dst ${CMAKE_CURRENT_BINARY_DIR}/resource_files.cpp --namespace ${BUNDLER_NAMESPACE} --mapname get_resource_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS bundler
)

add_library(static_files SHARED
  src/files.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/resource_files.cpp
  include/static_files/files.hpp
)

target_include_directories(static_files PUBLIC include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include)

find_package(ZLIB REQUIRED)
target_link_libraries(static_files ZLIB::ZLIB)

add_sanitizers(static_files)