#pragma once
#include "core/app_state_fwd.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace miximus::core::test_instrumentation {

class render_thread_delay_test_s
{
    std::optional<std::chrono::milliseconds> delay_;
    uint64_t                                 every_frames_{};
    uint64_t                                 rendered_frames_{};
    uint64_t                                 injections_{};

  public:
    explicit render_thread_delay_test_s(const app_state_s& app);

    void inject_before_render_frame();
    void publish_status(app_state_s* app) const;
};

} // namespace miximus::core::test_instrumentation
