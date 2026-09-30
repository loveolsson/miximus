#pragma once

#include "external_image_export.hpp"
#include "resource.hpp"

#include <atomic>

namespace miximus::gpu::detail {

struct external_image_export_s::state_s
{
    std::shared_ptr<texture_state_s> exported;
    external_image_s                 descriptor;
    std::atomic_bool                 foreign{};

    void initialize(const std::shared_ptr<device_state_s>& device, extent_s extent);
};

} // namespace miximus::gpu::detail
