#pragma once

#include "core/app_state_fwd.hpp"
#include "gpu/texture_fwd.hpp"
#include "nodes/source_info.hpp"
#include "render/pill_label.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace miximus::nodes::composite {

// Render-thread cache. At most one background task is outstanding per instance.
// Worker inputs own names, font metadata, and transfer leases; never node pointers.
class source_labels_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    source_labels_s();
    ~source_labels_s();
    source_labels_s(const source_labels_s& other)            = delete;
    source_labels_s& operator=(const source_labels_s& other) = delete;

    void
    update(core::app_state_s* app, std::span<const texture_source_info_s> sources, const render::pill_style_s& style);
    void clear();

    // Borrowed until the next update(), clear(), or destruction.
    const gpu::texture_s* find(std::string_view name) const;
};

} // namespace miximus::nodes::composite
