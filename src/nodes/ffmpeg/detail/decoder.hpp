#pragma once

#include "gpu/video_frame.hpp"
#include "utils/flicks.hpp"

#include <atomic>
#include <memory>
#include <span>
#include <string>

struct AVFrame;

namespace miximus::nodes::ffmpeg::detail {

class decoded_frame_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;
    friend class decoder_s;

    void prepare_layout(bool host);

  public:
    decoded_frame_s();
    ~decoded_frame_s();
    decoded_frame_s(decoded_frame_s&& other) noexcept;
    decoded_frame_s& operator=(decoded_frame_s&& other) noexcept;
    decoded_frame_s(const decoded_frame_s& other)            = delete;
    decoded_frame_s& operator=(const decoded_frame_s& other) = delete;

    utils::flicks          pts{};
    utils::flicks          duration{};
    gpu::vec2i_t           dimensions{};
    gpu::color_transform_s color;
    size_t                 buffer_bytes{};

    // Worker-only layout preparation. Host preparation rejects hardware surfaces;
    // unsupported GPU ingestion must retry a decoder, never download its frames.
    void                              prepare_host();
    void                              prepare_gpu();
    size_t                            gpu_storage_bytes() const;
    bool                              hardware_frame() const;
    bool                              vulkan_frame() const;
    AVFrame*                          native_frame() const;
    const gpu::video_buffer_header_s& component_header() const;

    void copy_to(std::span<std::byte> destination) const;
};

class hardware_devices_s;

class decoder_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    decoder_s(const std::string&           filename,
              bool                         hardware,
              int                          extra_frames,
              const std::atomic_bool&      stopped,
              std::span<const std::string> rejected_backends = {},
              gpu::device_s*               device            = nullptr,
              const hardware_devices_s*    devices           = nullptr);
    ~decoder_s();
    decoder_s(const decoder_s& other)            = delete;
    decoder_s& operator=(const decoder_s& other) = delete;
    decoder_s(decoder_s&& other)                 = delete;
    decoder_s& operator=(decoder_s&& other)      = delete;

    std::unique_ptr<decoded_frame_s> next(); // nullptr after draining EOF
    void                             rewind();
    std::string                      backend() const;
    std::string                      fallback_reason() const;
};

} // namespace miximus::nodes::ffmpeg::detail
