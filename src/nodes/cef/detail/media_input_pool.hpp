#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace miximus::nodes::cef::detail {

// Metadata only. Resource owners must retain their allocations until the matching
// ticket retires. This pool never infers GPU completion from IPC or object death.
class media_input_pool_s
{
  public:
    static constexpr size_t INPUT_COUNT = 8;
    static constexpr size_t MAX_DEPTH   = 8;

    struct ticket_s
    {
        size_t   input{};
        size_t   slot{};
        uint64_t generation{};
        uint64_t serial{};
        bool     operator==(const ticket_s&) const = default;
    };

    struct metrics_s
    {
        size_t   occupied{};
        size_t   high_water{};
        uint64_t admitted{};
        uint64_t capacity_drops{};
        uint64_t retired{};
    };

    explicit media_input_pool_s(size_t depth);

    // All methods use a short metadata lock; none allocates, calls a driver or waits.
    // One input's exhaustion never consumes another input's capacity.
    std::optional<ticket_s> acquire(size_t input);

    // Called only after successful native submission. Publication is not readiness.
    bool publish(const ticket_s& ticket);

    // Establish actual producer GPU completion (also required after cancellation).
    bool producer_finished(const ticket_s& ticket);

    // Only current-generation, published, producer-complete work may enter CEF.
    bool begin_consume(const ticket_s& ticket);

    // Actual consumer GPU retirement, NOT frame receipt/destruction or IPC ack.
    bool consumer_finished(const ticket_s& ticket);

    // Abandon a reservation only after all tentative/submitted GPU uses are gone.
    // Fails after publish; published work must cancel and finish normally.
    bool abandon(const ticket_s& ticket);
    bool cancel(const ticket_s& ticket);

    // Revokes delivery, not resource ownership. Old tickets remain occupied until
    // their outstanding producer/consumer GPU use retires. No overlapping pools.
    void      invalidate(size_t input);
    metrics_s metrics(size_t input) const;

  private:
    struct slot_s
    {
        ticket_s ticket;
        bool     occupied{};
        bool     published{};
        bool     producer_done{};
        bool     consuming{};
        bool     cancelled{};
    };

    struct input_s
    {
        uint64_t                      generation{1};
        std::array<slot_s, MAX_DEPTH> slots;
        metrics_s                     metrics;
    };

    size_t                           depth_;
    uint64_t                         next_serial_{};
    mutable std::mutex               mutex_;
    std::array<input_s, INPUT_COUNT> inputs_;

    slot_s*     find(const ticket_s& ticket);
    void        retire(slot_s& slot);
    static void validate_input(size_t input);
};

} // namespace miximus::nodes::cef::detail
