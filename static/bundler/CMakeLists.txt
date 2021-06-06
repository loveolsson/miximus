find_package(LibMagic REQUIRED)

add_executable(bundler
    src/main.cpp
)

target_include_directories(bundler PRIVATE 
    ${CMAKE_SOURCE_DIR}/submodules/gzip-hpp/include
    ${LIBMAGIC_INCLUDES}
)

target_link_libraries(bundler
    ZLIB::ZLIB
    Boost::filesystem
    ${LIBMAGIC_LIBRARIES}
)