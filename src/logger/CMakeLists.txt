add_library(logger
    logger.hpp 
    logger.cpp
)

target_link_libraries(logger 
PUBLIC
    spdlog::spdlog 
) 

target_include_directories(logger 
PUBLIC
    include
)