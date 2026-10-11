#pragma once

#include "gpu/texture.hpp"
#include "gpu/transfer/detail/memory_budget.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace miximus::nodes::ffmpeg::detail {

// Decode-worker only. Displayed/queued frames and recorded GPU work retain their
// storage and budget until both ownership and GPU completion permit retirement.
inline void retire_unused_textures(std::vector<std::shared_ptr<gpu::texture_s>>& pool,
                                   gpu::transfer::detail::memory_budget_s&       budget,
                                   size_t&                                       reserved,
                                   std::optional<gpu::vec2i_t>                   keep_dimensions = std::nullopt)
{
    for (auto entry = pool.begin(); entry != pool.end();) {
        const auto& texture = *entry;
        if (texture.use_count() != 1 || !texture->idle() ||
            (keep_dimensions && texture->dimensions() == *keep_dimensions)) {
            ++entry;
            continue;
        }

        const auto bytes =
            gpu::texture_s::estimate_storage_byte_size(texture->dimensions(), gpu::format_e::rgba_unorm16);
        entry = pool.erase(entry);
        reserved -= bytes;
        budget.release_memory(bytes);
    }
}

} // namespace miximus::nodes::ffmpeg::detail
