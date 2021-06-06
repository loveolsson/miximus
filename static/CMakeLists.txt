cmake_minimum_required(VERSION 3.16)

add_subdirectory(bundler)

add_custom_command(
  OUTPUT 
    ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp
  COMMAND $<TARGET_FILE:bundler> --src ./web/dist/ --dst ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp --namespace miximus::static_files --mapname get_web_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS bundler
)

add_custom_command(
  OUTPUT 
    ${CMAKE_CURRENT_BINARY_DIR}/shader_files.cpp
  COMMAND $<TARGET_FILE:bundler> --src ./shaders/ --dst ${CMAKE_CURRENT_BINARY_DIR}/shader_files.cpp --namespace miximus::static_files --mapname get_shader_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS bundler
)


add_library(static_files SHARED
  ${CMAKE_CURRENT_BINARY_DIR}/web_files.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/shader_files.cpp
  include/static_files/files.hpp
)

target_include_directories(static_files PUBLIC include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include)
target_link_libraries(static_files ZLIB::ZLIB)