#include "external_image_support.hpp"

#include <algorithm>
#include <array>

namespace miximus::gpu::detail {

external_image_import_support_s
probe_external_image_import(bool requested, [[maybe_unused]] std::span<const std::string_view> available_extensions)
{
    external_image_import_support_s result;
    result.requested = requested;
    if (!requested) {
        return result;
    }

#ifdef __linux__
    constexpr std::array<std::string_view, 5> required{
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
        "VK_EXT_queue_family_foreign",
        "VK_KHR_external_semaphore_fd",
    };

    for (const auto extension : required) {
        if (std::ranges::find(available_extensions, extension) == available_extensions.end()) {
            result.missing_support.emplace_back(extension);
        }
    }

    if (result.missing_support.empty()) {
        result.enabled = true;
        result.enabled_extensions.assign(required.begin(), required.end());
    }

#else
    result.missing_support.emplace_back("External image import is not qualified on this platform");
#endif
    return result;
}

} // namespace miximus::gpu::detail
