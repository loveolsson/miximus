add_library(logger
    include/logger/logger.hpp 
    src/logger.cpp
)

target_link_libraries(logger 
PUBLIC
    spdlog
) 

target_include_directories(logger 
PUBLIC
    include
)