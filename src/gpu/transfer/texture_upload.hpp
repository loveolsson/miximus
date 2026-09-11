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

namespace miximus::gpu::transfer {
namespace detail {
struct texture_upload_service_state_s;
struct texture_upload_slot_s;
struct texture_upload_stream_state_s;
} // namespace detail

struct texture_upload_config_s
{
    host_frame_layout_s host_layout;
    size_t              max_slots{3};
    size_t              initial_slots{};
    bool                generate_mip_maps{true};
    // Optional shared UNORM16 conversion target, allocated off the render thread.
    std::optional<sampling_e> conversion_sampling{};
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

// Stream selection owns ordinary uploads; a timed FIFO instead retains a lease
// for each frame so dropping a queued frame also retires its exact upload.
enum class upload_ownership_e
{
    stream,
    queued_frame,
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

    // Backend-owned stable memory, suitable for direct SDK DMA. A lease owns
    // one write cycle; submit only after the producer has finished its access.
    // Reusing an SDK buffer object must acquire a new lease, not reuse this address.
    std::span<std::byte> writable_host_bytes() const noexcept;
    texture_upload_id_s  upload_id() const noexcept;
    [[nodiscard]] bool   submit(upload_ownership_e ownership = upload_ownership_e::stream);
    explicit             operator bool() const noexcept { return slot_ != nullptr; }
};

class texture_upload_stream_s
{
    std::shared_ptr<detail::texture_upload_stream_state_s> state_;

    enum class availability_e
    {
        submitted,
        completed
    };
    texture_frame_ptr            select_upload(texture_upload_id_s upload_id, availability_e availability);
    texture_upload_wait_result_e wait_for_upload(texture_upload_id_s upload_id, availability_e availability) const;
    explicit texture_upload_stream_s(std::shared_ptr<detail::texture_upload_stream_state_s> state);
    friend class texture_upload_service_s;

  public:
    ~texture_upload_stream_s();

    texture_upload_stream_s(const texture_upload_stream_s&)            = delete;
    texture_upload_stream_s& operator=(const texture_upload_stream_s&) = delete;
    texture_upload_stream_s(texture_upload_stream_s&&)                 = delete;
    texture_upload_stream_s& operator=(texture_upload_stream_s&&)      = delete;

    [[nodiscard]] std::optional<texture_upload_lease_s> try_acquire_upload_buffer();
    [[nodiscard]] std::optional<texture_upload_lease_s> acquire_upload_buffer_for(std::chrono::milliseconds timeout);

    // Selects only a completed upload. Retain the returned frame through CPU
    // use; submitted GPU uses independently prevent slot reuse until complete.
    [[nodiscard]] texture_frame_ptr select_latest_completed_upload();
    [[nodiscard]] texture_frame_ptr select_latest_completed_upload_through(texture_upload_id_s upload_id);
    // Makes one exact completed upload current and discards other completed
    // uploads. This is intended for PTS-selected sources whose host buffers may
    // be returned in a different order from their transfer-slot acquisition.
    [[nodiscard]] texture_frame_ptr select_completed_upload(texture_upload_id_s upload_id);

    // FIFO consumers can hand the exact upload to the GPU before CPU completion
    // has been reported. Record wait_for(frame->upload_completion()) before use.
    // The slot remains unavailable to SDK producers until DMA and all uses finish.
    [[nodiscard]] texture_frame_ptr select_submitted_upload(texture_upload_id_s upload_id);
    texture_upload_wait_result_e    wait_for_upload_submission(texture_upload_id_s upload_id) const;
    // Returns the exact current frame for a timed-source repeat.
    [[nodiscard]] texture_frame_ptr retained_frame_for(texture_upload_id_s upload_id) const;

    // Retires an exact submitted upload which the render traversal no longer
    // needs. A queued transfer is reclaimed when its worker task completes;
    // an already-ready transfer is reclaimed immediately.
    void discard_upload(texture_upload_id_s upload_id);

    // Waits for one exact submitted upload. This does not select a different
    // completed texture; call the appropriate selection function after success.
    [[nodiscard]] texture_upload_wait_result_e wait_for_upload(texture_upload_id_s upload_id) const;
    texture_upload_id_s                        latest_completed_upload_id() const;
    texture_upload_id_s                        retained_upload_id() const;

    bool allocation_failed() const;
    auto configuration() const noexcept -> texture_upload_config_s;
};

class texture_upload_service_s
{
    std::shared_ptr<detail::texture_upload_service_state_s> state_;

  public:
    static constexpr size_t DEFAULT_MEMORY_BUDGET = size_t{1} << 30;

    explicit texture_upload_service_s(device_s& device, size_t memory_budget = DEFAULT_MEMORY_BUDGET);
    ~texture_upload_service_s();

    texture_upload_service_s(const texture_upload_service_s&)            = delete;
    texture_upload_service_s& operator=(const texture_upload_service_s&) = delete;
    texture_upload_service_s(texture_upload_service_s&&)                 = delete;
    texture_upload_service_s& operator=(texture_upload_service_s&&)      = delete;

    [[nodiscard]] std::shared_ptr<texture_upload_stream_s> create_stream(texture_upload_config_s config);

    size_t memory_usage() const noexcept;
    size_t memory_budget() const noexcept;
};

} // namespace miximus::gpu::transfer
