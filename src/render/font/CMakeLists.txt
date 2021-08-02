set(CMAKE_CXX_CLANG_TIDY ${MIXIMUS_CLANG_TIDY})

find_package(Freetype REQUIRED)

add_library(font
    registry.cpp
    registry.hpp
)

target_link_libraries(font 
PRIVATE
    Freetype::Freetype
    logger
) 

if(WIN32)
    target_sources(font PRIVATE
        registry_win.cpp
    )
else()
    target_sources(font PRIVATE
        registry_fontconfig.cpp
    )

    find_package(Fontconfig REQUIRED)

    target_link_libraries(font 
    PUBLIC
        Fontconfig::Fontconfig
    ) 
endif()

add_sanitizers(font)