#pragma once
#include "gpu/completion.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace miximus::gpu {
class device_s;
class recording_s;
} // namespace miximus::gpu

namespace miximus::nodes {
// Application-owned recording state. Nodes may record, flush, or defer publication;
// only the frame driver commits or abandons the whole frame.
class frame_commands_s
{
    gpu::device_s*                                      device_;
    std::unique_ptr<gpu::recording_s>                   recording_;
    gpu::completion_s                                   last_submission_;
    std::vector<std::function<void(gpu::completion_s)>> pending_outputs_;

  public:
    explicit frame_commands_s(gpu::device_s* device = nullptr);
    ~frame_commands_s();
    frame_commands_s(const frame_commands_s&)            = delete;
    frame_commands_s& operator=(const frame_commands_s&) = delete;
    gpu::recording_s& commands();
    gpu::completion_s submit_gpu();
    void              defer_output(std::function<void(gpu::completion_s)> publish);
    void              commit();
    void              abort() noexcept;

    class scope_s
    {
        frame_commands_s& commands_;

      public:
        explicit scope_s(frame_commands_s& commands)
            : commands_(commands)
        {
        }

        ~scope_s() { commands_.abort(); }

        scope_s(const scope_s&)            = delete;
        scope_s& operator=(const scope_s&) = delete;
    };
};
} // namespace miximus::nodes
