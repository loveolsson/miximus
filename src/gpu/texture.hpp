#pragma once

#include "resource_types.hpp"
#include "types.hpp"

#include <cstddef>
#include <memory>

namespace miximus::gpu {

class device_s;
class recording_s;
class texture_s
{
    std::shared_ptr<detail::texture_state_s> state_;

    explicit texture_s(std::shared_ptr<detail::texture_state_s> state);
    friend class device_s;
    friend class recording_s;
    friend class transfer::detail::cuda_transfer_s;
    friend class detail::dma_buf_copy_s;
    friend struct detail::presenter_state_s;

    channel_order_e channel_order_{channel_order_e::rgba};

  public:
    texture_s() = default;

    texture_s(device_s&          device,
              vec2i_t            dimensions,
              format_e           format,
              channel_order_e    mapping  = channel_order_e::rgba,
              sampling_e         sampling = sampling_e::mipmapped_linear,
              resource_sharing_e sharing  = resource_sharing_e::local);
    struct storage_format_info_s
    {
        size_t storage_bytes_per_texel;
        bool   integer;
    };

    static storage_format_info_s storage_format_info(format_e format);
    static int                   mip_map_level_count(vec2i_t dimensions, format_e format, sampling_e sampling);
    static size_t
    estimate_storage_byte_size(vec2i_t dimensions, format_e format, sampling_e sampling = sampling_e::mipmapped_linear);
    vec2i_t         dimensions() const;
    channel_order_e channel_order() const { return channel_order_; }
    void            clear(recording_s& recording) const;
    extent_s        extent() const;
    format_e        format() const;
    uint32_t        mip_levels() const;
    bool            idle() const;
    explicit        operator bool() const noexcept { return state_ != nullptr; }
};

} // namespace miximus::gpu
