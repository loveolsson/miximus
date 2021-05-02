add_library(config
    include/config/config.hpp
    src/config.cpp
)

target_link_libraries(config 
PRIVATE
    web_server
PUBLIC    
    messages
) 

target_include_directories(config 
PUBLIC
    include
)