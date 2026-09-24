#pragma once

#include "gpu/device.hpp"
#include "memory_budget.hpp"

#include <optional>
#include <utility>

namespace miximus::gpu::transfer::detail {

// Stream-owned conversion target; initialize/release only on the resource worker.
struct conversion_texture_s
{
    std::shared_ptr<texture_s> texture;
    size_t                     reserved_bytes{};
    void initialize(device_s& device, vec2i_t dimensions, std::optional<sampling_e> sampling, memory_budget_s& budget)
    {
        if (!sampling || texture) {
            return;
        }
        const auto bytes = texture_s::estimate_storage_byte_size(dimensions, format_e::rgba_unorm16, *sampling);
        if (!budget.reserve_memory(bytes)) {
            throw std::bad_alloc();
        }
        try {
            texture = std::make_shared<texture_s>(
                device, dimensions, format_e::rgba_unorm16, channel_order_e::rgba, *sampling);
            reserved_bytes = bytes;
        } catch (...) {
            budget.release_memory(bytes);
            throw;
        }
    }
    void release(memory_budget_s& budget)
    {
        texture.reset();
        budget.release_memory(std::exchange(reserved_bytes, 0));
    }
};
} // namespace miximus::gpu::transfer::detail
