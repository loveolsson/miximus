#pragma once

#include "gpu/recording.hpp"
#include "include/cef_browser.h"
#include "include/cef_process_message.h"
#include "nodes/cef/media_input_types.hpp"

#include <functional>
#include <memory>

namespace miximus::nodes::cef::detail {

// Owned before runtime_s and destroyed after its CefShutdown. Reservations and
// quarantines cover every session, including retired or failed producers.
class media_input_runtime_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;
    friend class media_input_session_s;

  public:
    media_input_runtime_s();
    ~media_input_runtime_s();
};

class media_input_session_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    media_input_session_s(gpu::device_s& device, std::shared_ptr<media_input_runtime_s> runtime);
    ~media_input_session_s();

    void attach(int browser_id);
    bool receive(const CefRefPtr<CefBrowser>&        browser,
                 const CefRefPtr<CefFrame>&          frame,
                 CefProcessId                        source,
                 const CefRefPtr<CefProcessMessage>& message);
    void revoke_context();
    void close();

    uint32_t              demand() const;
    bool                  idle() const;
    media_input_metrics_s metrics() const;

    // Render thread: stage a source selection/extent and record only when its
    // worker-owned generation is ready. Null source requests Chromium-owned transparent content without a GPU export.
    std::function<void()> record(size_t                input,
                                 gpu::recording_s&     commands,
                                 const gpu::texture_s* source,
                                 std::string_view      source_node,
                                 std::string_view      source_interface,
                                 int64_t               timestamp_us);
};

} // namespace miximus::nodes::cef::detail
