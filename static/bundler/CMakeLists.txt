add_executable(bundler
    src/main.cpp
)

find_package(ZLIB REQUIRED)


target_include_directories(bundler PRIVATE
    include
)

target_link_libraries(bundler
    ZLIB::ZLIB
    Boost::headers
)
