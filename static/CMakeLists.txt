cmake_minimum_required(VERSION 3.9)
set(CMAKE_CXX_CLANG_TIDY ${MIXIMUS_CLANG_TIDY})

add_subdirectory(bundler)

add_custom_command(
  OUTPUT 
    ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp
  COMMAND 
    $<TARGET_FILE:bundler> 
  ARGS 
    --src  ${CMAKE_SOURCE_DIR}/web/dist/ --dst ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp --namespace miximus::static_files --mapname get_web_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS bundler
)

add_custom_command(
  OUTPUT 
    ${CMAKE_CURRENT_BINARY_DIR}/resource_files.cpp
  COMMAND 
    $<TARGET_FILE:bundler> 
  ARGS 
    --src ${CMAKE_SOURCE_DIR}/resources/ --dst ${CMAKE_CURRENT_BINARY_DIR}/resource_files.cpp --namespace miximus::static_files --mapname get_resource_files
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