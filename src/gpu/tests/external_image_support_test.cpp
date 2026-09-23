#include "gpu/detail/external_image_support.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

namespace miximus::gpu::detail { namespace {

constexpr std::array<std::string_view, 5> complete_set{
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
    "VK_EXT_queue_family_foreign",
    "VK_KHR_external_semaphore_fd",
};

TEST(external_image_support, DisabledRequestDoesNotEnableOrRequireExtensions)
{
    for (auto available : {std::span<const std::string_view>{}, std::span<const std::string_view>{complete_set}}) {
        const auto result = probe_external_image_import(false, available);
        EXPECT_FALSE(result.requested);
        EXPECT_FALSE(result.enabled);
        EXPECT_TRUE(result.enabled_extensions.empty());
        EXPECT_TRUE(result.missing_support.empty());
    }
}

#ifdef __linux__
TEST(external_image_support, MissingAnyRequiredExtensionLeavesImportDisabled)
{
    for (size_t missing = 0; missing < complete_set.size(); ++missing) {
        std::vector<std::string_view> available(complete_set.begin(), complete_set.end());
        available.erase(available.begin() + static_cast<std::ptrdiff_t>(missing));
        const auto result = probe_external_image_import(true, available);
        EXPECT_TRUE(result.requested);
        EXPECT_FALSE(result.enabled);
        EXPECT_TRUE(result.enabled_extensions.empty());
        ASSERT_EQ(result.missing_support.size(), 1U);
        EXPECT_EQ(result.missing_support.front(), complete_set.at(missing));
    }
}

TEST(external_image_support, CompleteSetEnablesOnlyTheImportExtensions)
{
    std::vector<std::string_view> available(complete_set.begin(), complete_set.end());
    available.emplace_back("VK_KHR_swapchain");
    available.emplace_back(complete_set.front());
    auto result = probe_external_image_import(true, available);
    ASSERT_TRUE(result.enabled);
    EXPECT_TRUE(result.missing_support.empty());
    ASSERT_EQ(result.enabled_extensions.size(), complete_set.size());
    for (size_t index = 0; index < complete_set.size(); ++index) {
        EXPECT_EQ(result.enabled_extensions[index], complete_set.at(index));
    }
}
#else
TEST(external_image_support, UnqualifiedPlatformReportsUnavailable)
{
    const auto result = probe_external_image_import(true, complete_set);
    EXPECT_TRUE(result.requested);
    EXPECT_FALSE(result.enabled);
    EXPECT_TRUE(result.enabled_extensions.empty());
    EXPECT_FALSE(result.missing_support.empty());
}
#endif

}} // namespace miximus::gpu::detail
