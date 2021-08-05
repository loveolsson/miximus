add_library(logger
    logger.hpp 
    logger.cpp
)

find_package(spdlog REQUIRED)

target_link_libraries(logger 
PUBLIC
spdlog::spdlog_header_only
) 