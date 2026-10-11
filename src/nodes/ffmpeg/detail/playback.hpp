#pragma once

#include "gpu/transfer/detail/memory_budget.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/video_frame.hpp"
#include "nodes/ffmpeg/registry_fwd.hpp"
#include "utils/flicks.hpp"

#include <memory>
#include <string>

namespace miximus::nodes::ffmpeg::detail {

class hardware_devices_s;

struct playback_options_s
{
    std::string filename;
    bool        attempt_hardware_decode{true};
    int         buffered_frames{4};

    bool operator==(const playback_options_s& other) const = default;
};

struct playback_status_s
{
    bool        ready{};
    std::string backend;
    std::string transfer_path;
    std::string fallback_reason;
    std::string error;
    uint64_t    decoded_frames{};
    uint64_t    dropped_frames{};
    uint64_t    loops{};
    size_t      queued_frames{};
};

struct playback_frame_s
{
    std::shared_ptr<gpu::texture_s>                         gpu_texture;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    gpu::transfer::texture_upload_lease_s                   upload;
    gpu::transfer::texture_upload_id_s                      upload_id;
    gpu::color_transform_s                                  color;
    utils::flicks                                           pts;
    utils::flicks                                           duration;
};

class playback_session_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;
    friend class miximus::nodes::ffmpeg::ffmpeg_registry_s;

    void start();
    void stop();
    void join();

    playback_session_s(gpu::device_s&                                          device,
                       gpu::transfer::texture_upload_service_s&                uploads,
                       std::shared_ptr<gpu::transfer::detail::memory_budget_s> budget,
                       playback_options_s                                      options,
                       std::shared_ptr<const hardware_devices_s>               devices);

  public:
    ~playback_session_s();
    playback_session_s(const playback_session_s& other)            = delete;
    playback_session_s& operator=(const playback_session_s& other) = delete;
    playback_session_s(playback_session_s&& other)                 = delete;
    playback_session_s& operator=(playback_session_s&& other)      = delete;

    // Render thread: advance even without graph demand for always-play sources.
    void                                    advance(utils::flicks position);
    std::shared_ptr<const playback_frame_s> selected() const;
    void                                    rewind();
    playback_status_s                       status() const;
};

} // namespace miximus::nodes::ffmpeg::detail
