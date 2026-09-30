#include "gpu/detail/external_image_support.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace miximus::gpu::detail { namespace {

TEST(external_image_support, DisabledRequestDoesNotEnableOrRequireExtensions)
{
    EXPECT_EQ(probe_external_image_import(false, {}), external_image_import_support_e::not_requested);
    EXPECT_EQ(probe_external_image_import(false, external_image_import_extensions()),
              external_image_import_support_e::not_requested);
}

TEST(external_image_support, MissingAnyRequiredExtensionLeavesImportDisabled)
{
    const auto required = external_image_import_extensions();
    if (required.empty()) {
        EXPECT_EQ(probe_external_image_import(true, {}), external_image_import_support_e::unsupported_platform);
        return;
    }
    for (size_t missing = 0; missing < required.size(); ++missing) {
        std::vector<std::string_view> available(required.begin(), required.end());
        available.erase(available.begin() + static_cast<std::ptrdiff_t>(missing));
        EXPECT_EQ(probe_external_image_import(true, available), external_image_import_support_e::missing_extensions);
    }
}

TEST(external_image_support, CompleteSetEnablesImport)
{
    const auto required = external_image_import_extensions();
    if (required.empty()) {
        EXPECT_EQ(probe_external_image_import(true, {}), external_image_import_support_e::unsupported_platform);
        return;
    }
    std::vector<std::string_view> available(required.begin(), required.end());
    available.emplace_back("VK_KHR_swapchain");
    available.emplace_back(required.front());
    EXPECT_EQ(probe_external_image_import(true, available), external_image_import_support_e::supported);
}

}} // namespace miximus::gpu::detail
