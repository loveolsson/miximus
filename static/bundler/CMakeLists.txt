add_executable(bundler
    src/main.cpp
)

find_package(ZLIB REQUIRED)
find_package(fmt REQUIRED)


target_include_directories(bundler PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include
)

target_link_libraries(bundler
    fmt::fmt
    ZLIB::ZLIB
)