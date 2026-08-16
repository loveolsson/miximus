#pragma once
#include "gpu/texture_frame.hpp"
#include "gpu/transfer/texture_transfer.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"

#include <chrono>
#include <compare>
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
struct texture_upload_service_state_s;
struct texture_upload_slot_s;
struct texture_upload_stream_state_s;
} // namespace detail

struct texture_upload_config_s
{
    texture_transfer_layout_s transfer_layout;
    size_t                    max_slots{3};
    size_t                    initial_slots{};
    bool                      generate_mip_maps{true};
};

struct texture_upload_id_s
{
    uint64_t sequence{};

    auto     operator<=>(const texture_upload_id_s&) const = default;
    explicit operator bool() const noexcept { return sequence != 0; }
};

enum class texture_upload_wait_result_e : uint8_t
{
    ready,
    failed,
    stopped,
};

class texture_upload_lease_s
{
    std::shared_ptr<detail::texture_upload_stream_state_s> stream_;
    std::shared_ptr<detail::texture_upload_slot_s>         slot_;
    bool                                                   submitted_{};

    texture_upload_lease_s(std::shared_ptr<detail::texture_upload_stream_state_s> stream,
                           std::shared_ptr<detail::texture_upload_slot_s>         slot);

    friend class texture_upload_stream_s;

  public:
    texture_upload_lease_s() = default;
    ~texture_upload_lease_s();

    texture_upload_lease_s(const texture_upload_lease_s&)            = delete;
    texture_upload_lease_s& operator=(const texture_upload_lease_s&) = delete;
    texture_upload_lease_s(texture_upload_lease_s&&) noexcept;
    texture_upload_lease_s& operator=(texture_upload_lease_s&&) noexcept;

    std::span<std::byte> writable_host_bytes() const noexcept;
    texture_upload_id_s  upload_id() const noexcept;
    bool                 submit();
    explicit             operator bool() const noexcept { return slot_ != nullptr; }
};

class texture_upload_stream_s
{
    std::shared_ptr<detail::texture_upload_stream_state_s> state_;

    explicit texture_upload_stream_s(std::shared_ptr<detail::texture_upload_stream_state_s> state);
    friend class texture_upload_service_s;

  public:
    ~texture_upload_stream_s();

    texture_upload_stream_s(const texture_upload_stream_s&)            = delete;
    texture_upload_stream_s& operator=(const texture_upload_stream_s&) = delete;
    texture_upload_stream_s(texture_upload_stream_s&&)                 = delete;
    texture_upload_stream_s& operator=(texture_upload_stream_s&&)      = delete;

    std::optional<texture_upload_lease_s> try_acquire_upload_buffer();
    std::optional<texture_upload_lease_s> acquire_upload_buffer_for(std::chrono::milliseconds timeout);

    // Selects a frame but does not synchronize it. A consuming node calls
    // wait_for_upload_on_gpu() before use and publish_render_release_fence() in complete().
    // Polling consumers can retain their current frame while a newer upload is
    // incomplete.
    texture_frame_ptr select_latest_completed_upload();
    texture_frame_ptr select_latest_completed_upload_through(texture_upload_id_s upload_id);
    // Makes one exact completed upload current and discards other completed
    // uploads. This is intended for PTS-selected sources whose host buffers may
    // be returned in a different order from their transfer-slot acquisition.
    texture_frame_ptr select_completed_upload(texture_upload_id_s upload_id);
    // Returns the exact current frame for a timed-source repeat.
    texture_frame_ptr retained_frame_for(texture_upload_id_s upload_id) const;

    // Retires an exact submitted upload which the render traversal no longer
    // needs. A queued transfer is reclaimed when its worker task completes;
    // an already-ready transfer is reclaimed immediately.
    void discard_upload(texture_upload_id_s upload_id);

    // Waits for one exact submitted upload. This does not select a different
    // completed texture; call the appropriate selection function after success.
    texture_upload_wait_result_e wait_for_upload(texture_upload_id_s upload_id) const;
    texture_upload_id_s          latest_completed_upload_id() const;
    texture_upload_id_s          retained_upload_id() const;

    bool allocation_failed() const;
    auto configuration() const noexcept -> texture_upload_config_s;
};

class texture_upload_service_s
{
    std::shared_ptr<detail::texture_upload_service_state_s> state_;

  public:
    static constexpr size_t DEFAULT_MEMORY_BUDGET = size_t{1} << 30;

    explicit texture_upload_service_s(context_s* parent, size_t memory_budget = DEFAULT_MEMORY_BUDGET);
    ~texture_upload_service_s();

    texture_upload_service_s(const texture_upload_service_s&)            = delete;
    texture_upload_service_s& operator=(const texture_upload_service_s&) = delete;
    texture_upload_service_s(texture_upload_service_s&&)                 = delete;
    texture_upload_service_s& operator=(texture_upload_service_s&&)      = delete;

    std::shared_ptr<texture_upload_stream_s> create_stream(texture_upload_config_s config);

    size_t memory_usage() const noexcept;
    size_t memory_budget() const noexcept;
};

} // namespace miximus::gpu::transfer
