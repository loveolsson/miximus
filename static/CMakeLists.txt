cmake_minimum_required(VERSION 3.19)
project(static_files)

set (CMAKE_CXX_STANDARD 17)

add_library(static_files web/dist-bundle/dist.cpp)
target_include_directories(static_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include)
target_include_directories(static_files PRIVATE ${CMAKE_SOURCE_DIR}/submodules/zlib)
target_include_directories(static_files PRIVATE ${CMAKE_BINARY_DIR}/submodules/zlib)
target_link_libraries(static_files zlibstatic)
