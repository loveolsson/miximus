#pragma once
#include "gpu/framebuffer_fwd.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/readback_component_mapping.hpp"
#include "gpu/transfer/texture_readback_fwd.hpp"
#include "gpu/transfer/texture_transfer.hpp"
#include "utils/flicks.hpp"

#include <chrono>
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
struct texture_readback_service_state_s;
struct texture_readback_slot_s;
struct texture_readback_stream_state_s;
} // namespace detail

struct texture_readback_config_s
{
    texture_transfer_layout_s transfer_layout{.host_memory_access = host_memory_access_e::read_only};
    size_t                    max_slots{4};
    size_t                    initial_slots{};
};

struct texture_readback_stream_metrics_s
{
    size_t   slots{};
    size_t   free_slots{};
    size_t   rendering_slots{};
    size_t   queued_slots{};
    size_t   ready_slots{};
    size_t   cpu_reading_slots{};
    size_t   pending_allocations{};
    uint64_t render_target_acquire_misses{};
    uint64_t transfers_completed{};
    uint64_t transfer_failures{};
    int64_t  transfer_duration_total_us{};
    int64_t  transfer_duration_max_us{};
    bool     allocation_failed{};
};

class texture_readback_target_s
{
    std::shared_ptr<detail::texture_readback_stream_state_s> stream_;
    std::shared_ptr<detail::texture_readback_slot_s>         slot_;
    std::unique_ptr<framebuffer_s>                           framebuffer_;
    bool                                                     submitted_{};

    texture_readback_target_s(std::shared_ptr<detail::texture_readback_stream_state_s> stream,
                              std::shared_ptr<detail::texture_readback_slot_s>         slot);
    friend class texture_readback_stream_s;

  public:
    texture_readback_target_s() = default;
    ~texture_readback_target_s();

    texture_readback_target_s(const texture_readback_target_s&)            = delete;
    texture_readback_target_s& operator=(const texture_readback_target_s&) = delete;
    texture_readback_target_s(texture_readback_target_s&&) noexcept;
    texture_readback_target_s& operator=(texture_readback_target_s&&) noexcept;

    framebuffer_s*               framebuffer() const noexcept;
    readback_component_mapping_e readback_component_mapping() const noexcept;
    void                         set_program_target_time(utils::flicks program_target_time) noexcept;
    void                         submit();
    explicit                     operator bool() const noexcept { return slot_ != nullptr; }
};

class texture_readback_frame_s
{
    std::shared_ptr<detail::texture_readback_stream_state_s> stream_;
    std::shared_ptr<detail::texture_readback_slot_s>         slot_;

    texture_readback_frame_s(std::shared_ptr<detail::texture_readback_stream_state_s> stream,
                             std::shared_ptr<detail::texture_readback_slot_s>         slot);
    friend class texture_readback_stream_s;

  public:
    texture_readback_frame_s() = default;
    ~texture_readback_frame_s();

    texture_readback_frame_s(const texture_readback_frame_s&)            = delete;
    texture_readback_frame_s& operator=(const texture_readback_frame_s&) = delete;
    texture_readback_frame_s(texture_readback_frame_s&&) noexcept;
    texture_readback_frame_s& operator=(texture_readback_frame_s&&) noexcept;

    std::span<const std::byte> readable_host_bytes() const noexcept;
    utils::flicks              program_target_time() const noexcept;
    explicit                   operator bool() const noexcept { return slot_ != nullptr; }
};

class texture_readback_stream_s
{
    std::shared_ptr<detail::texture_readback_stream_state_s> state_;

    explicit texture_readback_stream_s(std::shared_ptr<detail::texture_readback_stream_state_s> state);
    friend class texture_readback_service_s;

  public:
    ~texture_readback_stream_s();

    texture_readback_stream_s(const texture_readback_stream_s&)            = delete;
    texture_readback_stream_s& operator=(const texture_readback_stream_s&) = delete;
    texture_readback_stream_s(texture_readback_stream_s&&)                 = delete;
    texture_readback_stream_s& operator=(texture_readback_stream_s&&)      = delete;

    // Render-thread API. Returns immediately if no target is available.
    std::optional<texture_readback_target_s> try_acquire_render_target();

    // CPU worker API. The returned lease keeps the buffer unavailable until
    // the external consumer has finished reading it.
    std::optional<texture_readback_frame_s> try_consume_oldest();
    std::optional<texture_readback_frame_s> try_consume_latest();

    bool allocation_failed() const;
    bool initial_slots_pending() const;
    bool wait_for_initial_slots(std::chrono::milliseconds timeout) const;
    auto metrics() const -> texture_readback_stream_metrics_s;
    auto configuration() const noexcept -> texture_readback_config_s;
};

class texture_readback_service_s
{
    std::shared_ptr<detail::texture_readback_service_state_s> state_;

  public:
    static constexpr size_t DEFAULT_MEMORY_BUDGET = size_t{1} << 30;

    explicit texture_readback_service_s(context_s* parent, size_t memory_budget = DEFAULT_MEMORY_BUDGET);
    ~texture_readback_service_s();

    texture_readback_service_s(const texture_readback_service_s&)            = delete;
    texture_readback_service_s& operator=(const texture_readback_service_s&) = delete;
    texture_readback_service_s(texture_readback_service_s&&)                 = delete;
    texture_readback_service_s& operator=(texture_readback_service_s&&)      = delete;

    std::shared_ptr<texture_readback_stream_s> create_stream(texture_readback_config_s config);

    size_t memory_usage() const noexcept;
    size_t memory_budget() const noexcept;
};

} // namespace miximus::gpu::transfer
