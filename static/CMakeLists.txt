cmake_minimum_required(VERSION 3.19)
project(static_files)

set (CMAKE_CXX_STANDARD 17)

add_subdirectory(src)

add_custom_command(
  OUTPUT ${SOURCE_FILES}
  COMMAND node ./bundler/index.js --src ./web/dist/ --dst ./src/web_files.cpp --namespace miximus::static_files --mapname web_files
  COMMAND node ./bundler/index.js --src ./shaders/ --dst ./src/shader_files.cpp --namespace miximus::static_files --mapname shader_files
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(static_files 
  ${SOURCE_FILES} 
  ${CMAKE_CURRENT_SOURCE_DIR}/include/static_files/files.hpp
)

target_include_directories(static_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/zlib)
target_include_directories(static_files PRIVATE ${CMAKE_BINARY_DIR}/submodules/zlib)
target_link_libraries(static_files zlibstatic)
