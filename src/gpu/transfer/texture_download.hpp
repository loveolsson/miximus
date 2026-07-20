#pragma once
#include "gpu/framebuffer_fwd.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_download_fwd.hpp"
#include "gpu/transfer/texture_transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace miximus::gpu {
class context_s;
}

namespace miximus::gpu::transfer {
namespace detail {
struct texture_download_service_state_s;
struct texture_download_slot_s;
struct texture_download_stream_state_s;
} // namespace detail

struct texture_download_desc_s
{
    texture_transfer_requirements_s requirements{.host_access = host_access_e::read_only};
    size_t                          max_slots{4};
};

class texture_download_target_s
{
    std::shared_ptr<detail::texture_download_stream_state_s> stream_;
    std::shared_ptr<detail::texture_download_slot_s>         slot_;
    std::unique_ptr<framebuffer_s>                           framebuffer_;
    bool                                                     submitted_{};

    texture_download_target_s(std::shared_ptr<detail::texture_download_stream_state_s> stream,
                              std::shared_ptr<detail::texture_download_slot_s>         slot);
    friend class texture_download_stream_s;

  public:
    texture_download_target_s() = default;
    ~texture_download_target_s();

    texture_download_target_s(const texture_download_target_s&)            = delete;
    texture_download_target_s& operator=(const texture_download_target_s&) = delete;
    texture_download_target_s(texture_download_target_s&&) noexcept;
    texture_download_target_s& operator=(texture_download_target_s&&) noexcept;

    framebuffer_s* framebuffer() const;
    void           set_tag(uint64_t tag);
    void           submit();
    explicit       operator bool() const { return slot_ != nullptr; }
};

class texture_download_frame_s
{
    std::shared_ptr<detail::texture_download_stream_state_s> stream_;
    std::shared_ptr<detail::texture_download_slot_s>         slot_;

    texture_download_frame_s(std::shared_ptr<detail::texture_download_stream_state_s> stream,
                             std::shared_ptr<detail::texture_download_slot_s>         slot);
    friend class texture_download_stream_s;

  public:
    texture_download_frame_s() = default;
    ~texture_download_frame_s();

    texture_download_frame_s(const texture_download_frame_s&)            = delete;
    texture_download_frame_s& operator=(const texture_download_frame_s&) = delete;
    texture_download_frame_s(texture_download_frame_s&&) noexcept;
    texture_download_frame_s& operator=(texture_download_frame_s&&) noexcept;

    std::span<const std::byte> bytes() const;
    uint64_t                   tag() const;
    explicit                   operator bool() const { return slot_ != nullptr; }
};

class texture_download_stream_s
{
    std::shared_ptr<detail::texture_download_stream_state_s> state_;

    explicit texture_download_stream_s(std::shared_ptr<detail::texture_download_stream_state_s> state);
    friend class texture_download_service_s;

  public:
    ~texture_download_stream_s();

    texture_download_stream_s(const texture_download_stream_s&)            = delete;
    texture_download_stream_s& operator=(const texture_download_stream_s&) = delete;
    texture_download_stream_s(texture_download_stream_s&&)                 = delete;
    texture_download_stream_s& operator=(texture_download_stream_s&&)      = delete;

    // Render-thread API. Returns immediately if no target is available.
    std::optional<texture_download_target_s> try_acquire();

    // CPU worker API. The returned lease keeps the buffer unavailable until
    // the external consumer has finished reading it.
    std::optional<texture_download_frame_s> try_consume_latest();

    bool allocation_failed() const;
    auto desc() const -> texture_download_desc_s;
};

class texture_download_service_s
{
    std::shared_ptr<detail::texture_download_service_state_s> state_;

  public:
    static constexpr size_t DEFAULT_MEMORY_BUDGET = size_t{1} << 30;

    explicit texture_download_service_s(context_s* parent, size_t memory_budget = DEFAULT_MEMORY_BUDGET);
    ~texture_download_service_s();

    texture_download_service_s(const texture_download_service_s&)            = delete;
    texture_download_service_s& operator=(const texture_download_service_s&) = delete;
    texture_download_service_s(texture_download_service_s&&)                 = delete;
    texture_download_service_s& operator=(texture_download_service_s&&)      = delete;

    std::shared_ptr<texture_download_stream_s> create_stream(texture_download_desc_s desc);

    size_t memory_usage() const;
    size_t memory_budget() const;
};

} // namespace miximus::gpu::transfer
