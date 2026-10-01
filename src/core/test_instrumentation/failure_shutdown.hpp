#pragma once

#ifdef MIXIMUS_ENABLE_FAILURE_TESTS
#include "core/app_state.hpp"
#include "utils/failure_shutdown.hpp"

#include <boost/asio/post.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace miximus::core::test_instrumentation {

// Explicit opt-in, available only in builds configured with BUILD_TESTING.
inline void inject_shutdown_failure(app_state_s& app)
{
    const auto* requested = std::getenv("MIXIMUS_TEST_SHUTDOWN_FAILURE");
    if (requested == nullptr) {
        return;
    }
    const std::string_view mode(requested);
    if (mode == "shutdown") {
        return;
    }
    if (mode == "render") {
        throw std::runtime_error("Injected render-thread failure");
    }
    if (mode == "configuration") {
        boost::asio::post(*app.cfg_executor(),
                          [] { throw std::runtime_error("Injected configuration-worker failure"); });
    } else if (mode == "park") {
        utils::fail_without_unwinding("Injected failure with retained main-thread ownership");
    } else {
        throw std::invalid_argument("Unknown MIXIMUS_TEST_SHUTDOWN_FAILURE mode");
    }
}

inline void inject_teardown_failure()
{
    const auto* requested = std::getenv("MIXIMUS_TEST_SHUTDOWN_FAILURE");
    if (requested != nullptr && std::string_view(requested) == "shutdown") {
        throw std::runtime_error("Injected failure during shutdown");
    }
}

} // namespace miximus::core::test_instrumentation
#endif
