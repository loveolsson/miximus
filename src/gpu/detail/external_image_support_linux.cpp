#include "external_image_support.hpp"

#include <array>

namespace miximus::gpu::detail {
namespace {

constexpr std::array<std::string_view, 5> REQUIRED_EXTENSIONS{
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
    "VK_EXT_queue_family_foreign",
    "VK_KHR_external_semaphore_fd",
};

} // namespace

std::span<const std::string_view> external_image_import_extensions() { return REQUIRED_EXTENSIONS; }

} // namespace miximus::gpu::detail
