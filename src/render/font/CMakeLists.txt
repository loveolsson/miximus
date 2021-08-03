target_sources(render 
PRIVATE
    registry.cpp
    registry.hpp
    font_instance.cpp
    font_instance.hpp
    font_loader.cpp
    font_loader.hpp
    font_info.hpp
)

find_package(Freetype REQUIRED)

target_link_libraries(render 
PRIVATE
    Freetype::Freetype
) 

if(WIN32)
    target_sources(render PRIVATE
        registry_win.cpp
    )
else()
    find_package(Fontconfig REQUIRED)
    
    target_link_libraries(render 
    PRIVATE
        Fontconfig::Fontconfig
    ) 

    target_sources(render PRIVATE
        registry_fontconfig.cpp
    )
endif()