#pragma once

#include "logger/logger.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace miximus::utils {

// Own on the main thread for the application's lifetime. Windows execution
// requirements are thread-local, so this guard must not move between threads.
// Other platforms retain their existing power-management behavior.
class sleep_inhibitor_s
{
#ifdef _WIN32
    EXECUTION_STATE previous_state_{};
#endif

  public:
    sleep_inhibitor_s()
    {
#ifdef _WIN32
        previous_state_ = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        if (previous_state_ == 0) {
            getlog("app")->warn("Could not inhibit automatic system and display sleep");
        } else {
            getlog("app")->info("Automatic system and display sleep inhibited while Miximus is running");
        }
#endif
    }

    sleep_inhibitor_s(const sleep_inhibitor_s&)            = delete;
    sleep_inhibitor_s& operator=(const sleep_inhibitor_s&) = delete;
    sleep_inhibitor_s(sleep_inhibitor_s&&)                 = delete;
    sleep_inhibitor_s& operator=(sleep_inhibitor_s&&)      = delete;

    ~sleep_inhibitor_s()
    {
#ifdef _WIN32
        if (previous_state_ != 0) {
            if (SetThreadExecutionState(ES_CONTINUOUS | previous_state_) == 0) {
                getlog("app")->warn("Could not restore automatic sleep policy");
            } else {
                getlog("app")->info("Application sleep inhibition released");
            }
        }
#endif
    }
};

} // namespace miximus::utils
