#pragma once

#include "gpu/transfer/texture_upload_fwd.hpp"
#include "registry_fwd.hpp"

#include <memory>

namespace miximus::gpu {

class device_s;
}

namespace miximus::nodes::ffmpeg {

namespace detail {

class playback_session_s;
struct playback_options_s;

} // namespace detail

// Owns playback sessions and drains asynchronous retirement before GPU teardown.
class ffmpeg_registry_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    explicit ffmpeg_registry_s(gpu::device_s& device, gpu::transfer::texture_upload_service_s& uploads);
    ~ffmpeg_registry_s();
    ffmpeg_registry_s(const ffmpeg_registry_s& other)            = delete;
    ffmpeg_registry_s& operator=(const ffmpeg_registry_s& other) = delete;
    ffmpeg_registry_s(ffmpeg_registry_s&& other)                 = delete;
    ffmpeg_registry_s& operator=(ffmpeg_registry_s&& other)      = delete;

    std::shared_ptr<detail::playback_session_s> create(detail::playback_options_s options);
    void                                        retire(std::shared_ptr<detail::playback_session_s> session);
};

} // namespace miximus::nodes::ffmpeg
