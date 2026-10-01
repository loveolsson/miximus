#pragma once

#include "gpu/device.hpp"

#include <span>
#include <string_view>

namespace miximus::gpu::detail {

// Null-terminated extension names with static lifetime.
std::span<const std::string_view> external_image_import_extensions();

// Called only for the already-selected device. Missing import support is not a
// reason to select a different device or reject ordinary rendering.
external_image_import_support_e probe_external_image_import(bool                              requested,
                                                            std::span<const std::string_view> available_extensions);

} // namespace miximus::gpu::detail
