add_library(stb
    stb_impl.cpp
)

target_compile_options(stb PRIVATE -w)

target_include_directories(stb SYSTEM
PUBLIC
    ${CMAKE_SOURCE_DIR}/submodules/stb
)
