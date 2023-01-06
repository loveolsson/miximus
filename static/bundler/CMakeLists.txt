add_executable(bundler
    src/main.cpp
)

find_package(ZLIB REQUIRED)

target_include_directories(bundler PRIVATE 
    ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include
)

target_link_libraries(bundler
    ZLIB::ZLIB
)