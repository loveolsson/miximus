#include "external_image_support.hpp"

#include <algorithm>

namespace miximus::gpu::detail {

external_image_import_support_e probe_external_image_import(bool                              requested,
                                                            std::span<const std::string_view> available_extensions)
{
    if (!requested) {
        return external_image_import_support_e::not_requested;
    }

    const auto required = external_image_import_extensions();
    if (required.empty()) {
        return external_image_import_support_e::unsupported_platform;
    }
    for (const auto extension : required) {
        if (std::ranges::find(available_extensions, extension) == available_extensions.end()) {
            return external_image_import_support_e::missing_extensions;
        }
    }

    return external_image_import_support_e::supported;
}

} // namespace miximus::gpu::detail
