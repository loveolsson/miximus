cmake_minimum_required(VERSION 3.16)

set (CMAKE_CXX_STANDARD 17)

add_subdirectory(src)

add_custom_command(
  OUTPUT ${SOURCE_FILES}
  COMMAND node ./bundler/index.js --src ./web/dist/ --dst ./src/web_files.cpp --namespace miximus::static_files --mapname get_web_files
  COMMAND node ./bundler/index.js --src ./shaders/ --dst ./src/shader_files.cpp --namespace miximus::static_files --mapname get_shader_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(static_files SHARED
  ${SOURCE_FILES} 
  include/static_files/files.hpp
)

target_compile_definitions(static_files PRIVATE LIBRARY_EXPORTS)

target_include_directories(static_files PUBLIC include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include)
target_link_libraries(static_files ZLIB::ZLIB)

