add_library(shutdown_watchdog
    shutdown_watchdog.hpp
    shutdown_watchdog.cpp
    failure_shutdown.hpp
    settings_file.cpp
)

target_link_libraries(shutdown_watchdog PRIVATE logger Threads::Threads)

set(MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS 10)
if(SANITIZE_ADDRESS OR SANITIZE_MEMORY OR SANITIZE_THREAD OR SANITIZE_UNDEFINED)
    set(MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS 60)
endif()

target_compile_definitions(shutdown_watchdog
PRIVATE
    MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS=${MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS}
)

add_sanitizers(shutdown_watchdog)

add_library(shutdown_signal shutdown_signal.hpp)
if(WIN32)
    target_sources(shutdown_signal PRIVATE shutdown_signal_win.cpp)
else()
    target_sources(shutdown_signal PRIVATE shutdown_signal_linux.cpp)
endif()

target_link_libraries(shutdown_signal PRIVATE shutdown_watchdog)
add_sanitizers(shutdown_signal)

if(BUILD_TESTING)
    add_executable(shutdown_test tests/failure_shutdown_test.cpp)
    target_link_libraries(shutdown_test PRIVATE shutdown_watchdog GTest::gtest_main)
    add_sanitizers(shutdown_test)
    gtest_discover_tests(shutdown_test)
endif()
