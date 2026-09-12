#include "texture.hpp"

#include "detail/device.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace miximus::gpu {
namespace {
size_t checked_add(size_t lhs, size_t rhs)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        throw std::overflow_error("texture storage byte size overflow");
    }
    return lhs + rhs;
}

size_t checked_multiply(size_t lhs, size_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        throw std::overflow_error("texture storage byte size overflow");
    }
    return lhs * rhs;
}
} // namespace

texture_s::storage_format_info_s texture_s::storage_format_info(format_e storage_format)
{
    switch (storage_format) {
        case format_e::rgba16_float:
        case format_e::rgba_unorm16:
            return {.storage_bytes_per_texel = 8, .integer = false};
        case format_e::rgba_unorm8:
            return {.storage_bytes_per_texel = 4, .integer = false};
        case format_e::r32_uint:
            return {.storage_bytes_per_texel = 4, .integer = true};
    }
    throw std::invalid_argument("Invalid texture storage format");
}

texture_s::texture_s(device_s&          device,
                     vec2i_t            dimensions,
                     format_e           format,
                     channel_order_e    mapping,
                     sampling_e         sampling,
                     resource_sharing_e sharing)
    : texture_s(device.create_texture(
          {.width = static_cast<uint32_t>(dimensions.x), .height = static_cast<uint32_t>(dimensions.y)},
          format,
          sampling,
          sharing))
{
    channel_order_ = mapping;
}

vec2i_t texture_s::dimensions() const
{
    const auto size = extent();
    return {size.width, size.height};
}

texture_s::texture_s(std::shared_ptr<detail::texture_state_s> state)
    : state_(std::move(state))
{
}

extent_s texture_s::extent() const
{
    if (!state_) {
        throw std::logic_error("empty image");
    }
    return state_->extent;
}

format_e texture_s::format() const
{
    if (!state_) {
        throw std::logic_error("empty image");
    }
    return state_->format;
}

uint32_t texture_s::mip_levels() const { return state_ ? state_->mip_levels : 0; }
bool     texture_s::idle() const
{
    return !state_ ||
           (state_->recording_uses.load() == 0 && state_->last_use_timeline_value.load() <= state_->owner->completed());
}

void texture_s::clear(recording_s& recording) const { recording.clear(*this); }

int texture_s::mip_map_level_count(vec2i_t dimensions, format_e storage_format, sampling_e sampling)
{
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        throw std::invalid_argument("texture dimensions must be positive");
    }

    if (storage_format_info(storage_format).integer || sampling != sampling_e::mipmapped_linear) {
        return 1;
    }

    const auto maximum_dimension = static_cast<unsigned>(std::max(dimensions.x, dimensions.y));
    return std::bit_width(maximum_dimension);
}

size_t texture_s::estimate_storage_byte_size(vec2i_t dimensions, format_e storage_format, sampling_e sampling)
{
    const auto mip_map_levels = mip_map_level_count(dimensions, storage_format, sampling);

    const auto info   = storage_format_info(storage_format);
    auto       width  = static_cast<size_t>(dimensions.x);
    auto       height = static_cast<size_t>(dimensions.y);
    size_t     byte_size{};

    for (int level = 0; level < mip_map_levels; ++level) {
        const auto texel_count = checked_multiply(width, height);
        byte_size              = checked_add(byte_size, checked_multiply(texel_count, info.storage_bytes_per_texel));
        width                  = std::max<size_t>(1, width / 2);
        height                 = std::max<size_t>(1, height / 2);
    }

    return byte_size;
}

namespace detail {

texture_state_s::~texture_state_s()
{
    if (image == nullptr) {
        return;
    }
    const auto allocator     = owner->allocator;
    const auto device        = owner->device;
    const auto destroy_view  = owner->vk.vkDestroyImageView;
    const auto destroy_image = owner->vk.vkDestroyImage;
    const auto free_memory   = owner->vk.vkFreeMemory;
    owner->retire(last_use_timeline_value.load(),
                  [allocator,
                   device,
                   destroy_view,
                   destroy_image,
                   free_memory,
                   external   = external_memory,
                   handle     = image,
                   image_view = view,
                   sampled    = sampled_view,
                   memory     = allocation] {
                      if (sampled && sampled != image_view) {
                          destroy_view(device, sampled, nullptr);
                      }
                      if (image_view) {
                          destroy_view(device, image_view, nullptr);
                      }
                      if (external) {
                          destroy_image(device, handle, nullptr);
                          free_memory(device, external, nullptr);
                      } else if (memory) {
                          vmaDestroyImage(allocator, handle, memory);
                      } else {
                          destroy_image(device, handle, nullptr);
                      }
                  });
}
} // namespace detail

} // namespace miximus::gpu
