target_sources(render 
PRIVATE
    font_registry.cpp
    font_registry.hpp
    font_registry_fwd.hpp
    font_instance.cpp
    font_instance.hpp
    font_instance_fwd.hpp
    font_loader.cpp
    font_loader.hpp
    font_loader_fwd.hpp
    font_info.hpp
    font_info_fwd.hpp
)

find_package(Freetype REQUIRED)

target_link_libraries(render 
PUBLIC
    Freetype::Freetype
) 

if(WIN32)
    target_sources(render PRIVATE
        font_registry_win.cpp
    )
else()
    find_package(Fontconfig REQUIRED)
    
    target_link_libraries(render 
    PRIVATE
        Fontconfig::Fontconfig
    ) 

    target_sources(render PRIVATE
        font_registry_fontconfig.cpp
    )
endif()
