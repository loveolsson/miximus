#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace miximus::stb {

enum class image_channels_e : int
{
    grayscale       = 1,
    grayscale_alpha = 2,
    rgb             = 3,
    rgba            = 4,
};

namespace detail {
struct image_deleter_s
{
    void operator()(uint8_t* data) const noexcept;
};
} // namespace detail

class decoded_image_s
{
    int                                               width_;
    int                                               height_;
    int                                               source_channels_;
    image_channels_e                                  channels_;
    std::unique_ptr<uint8_t, detail::image_deleter_s> data_;

    decoded_image_s(int                                               width,
                    int                                               height,
                    int                                               source_channels,
                    image_channels_e                                  channels,
                    std::unique_ptr<uint8_t, detail::image_deleter_s> data);
    friend decoded_image_s decode_image(std::span<const std::byte> encoded, image_channels_e channels);

  public:
    decoded_image_s(const decoded_image_s&)            = delete;
    decoded_image_s(decoded_image_s&&)                 = default;
    decoded_image_s& operator=(const decoded_image_s&) = delete;
    decoded_image_s& operator=(decoded_image_s&&)      = default;
    ~decoded_image_s()                                 = default;

    int                width() const { return width_; }
    int                height() const { return height_; }
    int                source_channels() const { return source_channels_; }
    image_channels_e   channels() const { return channels_; }
    std::span<uint8_t> pixels() noexcept
    {
        if (!data_) {
            return {};
        }
        return {data_.get(),
                static_cast<size_t>(width_) * static_cast<size_t>(height_) * static_cast<size_t>(channels_)};
    }

    std::span<const uint8_t> pixels() const noexcept
    {
        if (!data_) {
            return {};
        }
        return {data_.get(),
                static_cast<size_t>(width_) * static_cast<size_t>(height_) * static_cast<size_t>(channels_)};
    }
};

decoded_image_s decode_image(std::span<const std::byte> encoded, image_channels_e channels);

} // namespace miximus::stb
