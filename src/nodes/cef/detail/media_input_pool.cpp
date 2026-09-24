#include "media_input_pool.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace miximus::nodes::cef::detail {

media_input_pool_s::media_input_pool_s(size_t depth)
    : depth_(depth)
{
    if (depth == 0 || depth > MAX_DEPTH) {
        throw std::invalid_argument("Browser media input depth must be in [1, 8]");
    }
}

void media_input_pool_s::validate_input(size_t input)
{
    if (input >= INPUT_COUNT) {
        throw std::out_of_range("Browser media input index must be in [0, 7]");
    }
}

std::optional<media_input_pool_s::ticket_s> media_input_pool_s::acquire(size_t input)
{
    validate_input(input);
    const std::lock_guard lock(mutex_);
    auto&                 entry = inputs_[input];
    for (size_t index = 0; index < depth_; ++index) {
        auto& slot = entry.slots[index];
        if (slot.occupied) {
            continue;
        }
        if (next_serial_ == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("Browser media input ticket space exhausted");
        }
        slot = {
            .ticket = {input, index, entry.generation, ++next_serial_},
              .occupied = true
        };
        ++entry.metrics.occupied;
        ++entry.metrics.admitted;
        entry.metrics.high_water = std::max(entry.metrics.high_water, entry.metrics.occupied);
        return slot.ticket;
    }
    ++entry.metrics.capacity_drops;
    return std::nullopt;
}

media_input_pool_s::slot_s* media_input_pool_s::find(const ticket_s& ticket)
{
    if (ticket.input >= INPUT_COUNT || ticket.slot >= depth_) {
        return nullptr;
    }
    auto& slot = inputs_[ticket.input].slots[ticket.slot];
    return slot.occupied && slot.ticket == ticket ? &slot : nullptr;
}

void media_input_pool_s::retire(slot_s& slot)
{
    auto& metrics = inputs_[slot.ticket.input].metrics;
    --metrics.occupied;
    ++metrics.retired;
    slot = {};
}

bool media_input_pool_s::publish(const ticket_s& ticket)
{
    const std::lock_guard lock(mutex_);
    auto*                 slot = find(ticket);
    if (!slot || slot->published) {
        return false;
    }
    // Even revoked work can have been submitted before invalidation raced with
    // publication. Record ownership; begin_consume still rejects its delivery.
    slot->published = true;
    return true;
}

bool media_input_pool_s::producer_finished(const ticket_s& ticket)
{
    const std::lock_guard lock(mutex_);
    auto*                 slot = find(ticket);
    if (!slot || !slot->published || slot->producer_done) {
        return false;
    }
    slot->producer_done = true;
    if (slot->cancelled) {
        retire(*slot);
    }
    return true;
}

bool media_input_pool_s::begin_consume(const ticket_s& ticket)
{
    const std::lock_guard lock(mutex_);
    auto*                 slot = find(ticket);
    if (!slot || !slot->producer_done || slot->consuming || slot->cancelled) {
        return false;
    }
    slot->consuming = true;
    return true;
}

bool media_input_pool_s::consumer_finished(const ticket_s& ticket)
{
    const std::lock_guard lock(mutex_);
    auto*                 slot = find(ticket);
    if (!slot || !slot->consuming) {
        return false;
    }
    retire(*slot);
    return true;
}

bool media_input_pool_s::abandon(const ticket_s& ticket)
{
    const std::lock_guard lock(mutex_);
    auto*                 slot = find(ticket);
    if (!slot || slot->published) {
        return false;
    }
    retire(*slot);
    return true;
}

bool media_input_pool_s::cancel(const ticket_s& ticket)
{
    const std::lock_guard lock(mutex_);
    auto*                 slot = find(ticket);
    if (!slot || slot->cancelled) {
        return false;
    }
    slot->cancelled = true;
    if (slot->producer_done && !slot->consuming) {
        retire(*slot);
    }
    return true;
}

void media_input_pool_s::invalidate(size_t input)
{
    validate_input(input);
    const std::lock_guard lock(mutex_);
    auto&                 entry = inputs_[input];
    if (entry.generation == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("Browser media input generation space exhausted");
    }
    ++entry.generation;
    for (auto& slot : entry.slots) {
        if (slot.occupied) {
            slot.cancelled = true;
            if (slot.producer_done && !slot.consuming) {
                retire(slot);
            }
        }
    }
}

media_input_pool_s::metrics_s media_input_pool_s::metrics(size_t input) const
{
    validate_input(input);
    const std::lock_guard lock(mutex_);
    return inputs_[input].metrics;
}

} // namespace miximus::nodes::cef::detail
