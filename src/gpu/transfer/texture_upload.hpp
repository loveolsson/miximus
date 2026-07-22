#pragma once
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_transfer.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"

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
struct texture_upload_service_state_s;
struct texture_upload_slot_s;
struct texture_upload_stream_state_s;
} // namespace detail

struct texture_upload_desc_s
{
    texture_transfer_requirements_s requirements;
    size_t                          max_slots{3};
    bool                            generate_mip_maps{true};
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

    std::span<std::byte> bytes() const;
    uint64_t             version() const;
    void                 submit();
    explicit             operator bool() const { return slot_ != nullptr; }
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

    std::optional<texture_upload_lease_s> try_acquire();
    std::optional<texture_upload_lease_s> acquire_for(std::chrono::milliseconds timeout);

    // Called on the render thread with its GL context current. Polling consumers
    // can retain their current texture while a newer upload remains incomplete.
    texture_s* consume_latest();
    texture_s* consume_through(uint64_t version);

    // Waits for one exact submitted version. This does not make a different
    // completed texture current; call consume_through(version) after success.
    texture_upload_wait_result_e wait_until_ready(uint64_t version) const;
    uint64_t                     latest_ready_version() const;
    uint64_t                     current_version() const;

    bool allocation_failed() const;
    auto desc() const -> texture_upload_desc_s;
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

    std::shared_ptr<texture_upload_stream_s> create_stream(texture_upload_desc_s desc);

    size_t memory_usage() const;
    size_t memory_budget() const;
};

} // namespace miximus::gpu::transfer
