#pragma once

#include "recording.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace miximus::gpu {

class recording_unavailable_s : public std::runtime_error
{
  public:
    explicit recording_unavailable_s(const char* reason = "GPU recording capacity is busy")
        : std::runtime_error(reason)
    {
    }
};

struct device_options_s
{
    std::string device_uuid{};
    bool        validation{};
    bool        presentation{};
    bool        use_cuda{};
    uint32_t    max_recordings{8};
    uint32_t    descriptor_page_size{512};
    // Optional import capability; never changes device/queue selection.
    bool external_image_import{};
};

struct external_image_import_support_s
{
    bool                     requested{};
    bool                     enabled{};
    std::vector<std::string> enabled_extensions;
    std::vector<std::string> missing_support;
};

class device_s
{
    std::shared_ptr<detail::device_state_s> state_;
    std::unique_ptr<recording_context_s>    default_context_;
    friend struct detail::presenter_state_s;
    friend class transfer::detail::cuda_transfer_s;
    friend class detail::dma_buf_export_s;

  public:
    explicit device_s(const device_options_s& options = {});
    ~device_s();
    device_s(const device_s&)            = delete;
    device_s& operator=(const device_s&) = delete;

    texture_s           create_texture(extent_s           extent,
                                       format_e           format   = format_e::rgba_unorm16,
                                       sampling_e         sampling = sampling_e::linear,
                                       resource_sharing_e sharing  = resource_sharing_e::local);
    buffer_s            create_buffer(size_t             bytes,
                                      host_access_e      access,
                                      size_t             alignment = 1,
                                      resource_sharing_e sharing   = resource_sharing_e::local);
    recording_context_s create_recording_context(uint32_t capacity = 8);

    // The device's default context is reserved for its owning render thread.
    // An exhausted context returns immediately; workers have independent contexts.
    std::unique_ptr<recording_s>    try_record();
    void                            collect();
    std::string                     diagnostics_json() const;
    uint64_t                        validation_errors() const noexcept;
    bool                            uses_cuda_transfers() const noexcept;
    external_image_import_support_s external_image_import_support() const;
};

} // namespace miximus::gpu
