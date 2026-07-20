add_library(stb
    image.cpp
    image.hpp
)

target_compile_options(stb PRIVATE -w)

target_include_directories(stb SYSTEM
PRIVATE
    ${CMAKE_SOURCE_DIR}/submodules/stb
)

add_sanitizers(stb)
