add_library(shutdown_watchdog
    shutdown_watchdog.hpp
    shutdown_watchdog.cpp
)

target_link_libraries(shutdown_watchdog PRIVATE logger)

set(MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS 10)
if(SANITIZE_ADDRESS OR SANITIZE_MEMORY OR SANITIZE_THREAD OR SANITIZE_UNDEFINED)
    set(MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS 60)
endif()

target_compile_definitions(shutdown_watchdog
PRIVATE
    MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS=${MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS}
)

add_sanitizers(shutdown_watchdog)
