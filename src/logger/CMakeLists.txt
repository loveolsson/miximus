add_library(logger
    logger.hpp
    logger.cpp
)

target_link_libraries(logger
PUBLIC
spdlog::spdlog_header_only
)

add_sanitizers(logger)
