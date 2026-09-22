#include "gpu/device.hpp"
#include "logger/logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace miximus::gpu { namespace {

using namespace std::chrono_literals;

device_options_s test_options()
{
    device_options_s options;
    // Read-only test configuration; the process environment is never mutated.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* uuid = std::getenv("MIXIMUS_VULKAN_DEVICE")) {
        options.device_uuid = uuid;
    }
    return options;
}

class external_image_device : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        if (!spdlog::get("gpu")) {
            logger::init_loggers(spdlog::level::warn);
        }
    }
};

TEST_F(external_image_device, OptionalImportPreservesDeviceAndOrdinaryRendering)
{
    auto       options = test_options();
    device_s   baseline(options);
    const auto before = nlohmann::json::parse(baseline.diagnostics_json());
    EXPECT_FALSE(baseline.external_image_import_support().requested);
    EXPECT_FALSE(baseline.external_image_import_support().enabled);

    options.external_image_import = true;
    device_s   importing(options);
    const auto after = nlohmann::json::parse(importing.diagnostics_json());
    EXPECT_EQ(before.at("selected_uuid"), after.at("selected_uuid"));
    EXPECT_EQ(before.at("separate_present_queue"), after.at("separate_present_queue"));
    EXPECT_EQ(before.at("swapchain_maintenance"), after.at("swapchain_maintenance"));
    EXPECT_EQ(before.at("present_wait"), after.at("present_wait"));
    EXPECT_FALSE(importing.uses_cuda_transfers());
    const auto support = importing.external_image_import_support();
    EXPECT_TRUE(support.requested);

    bool available = false;
#ifdef __linux__
    for (const auto& candidate : after.at("devices")) {
        if (candidate.at("uuid") != after.at("selected_uuid")) {
            continue;
        }
        available = true;
        for (const auto* name : {"VK_KHR_external_memory_fd",
                                 "VK_EXT_external_memory_dma_buf",
                                 "VK_EXT_image_drm_format_modifier",
                                 "VK_EXT_queue_family_foreign",
                                 "VK_KHR_external_semaphore_fd"}) {
            bool found = false;
            for (const auto& extension : candidate.at("extensions")) {
                if (extension == name) {
                    found = true;
                }
            }
            available &= found;
        }
    }
#endif
    EXPECT_EQ(support.enabled, available);
    EXPECT_EQ(support.enabled_extensions.size(), available ? 5U : 0U);
    EXPECT_EQ(support.missing_support.empty(), available);

    auto texture = importing.create_texture({32, 32});
    auto context = importing.create_recording_context();
    auto record  = context.try_record();
    ASSERT_TRUE(record);
    record->clear(texture);
    EXPECT_EQ(record->submit().wait(5s), wait_result_e::ready);
    EXPECT_EQ(importing.validation_errors(), 0U);
    EXPECT_EQ(baseline.validation_errors(), 0U);
}

#ifdef __linux__
TEST_F(external_image_device, ImportAndCudaRequestsCanCoexist)
{
    auto options                  = test_options();
    options.use_cuda              = true;
    options.external_image_import = true;
    // CUDA qualification may legitimately fail on this machine/build. Import
    // support must remain independent, with no duplicate device extensions.
    device_s device(options);
    EXPECT_TRUE(device.external_image_import_support().requested);
    auto texture = device.create_texture({32, 32});
    auto record  = device.try_record();
    ASSERT_TRUE(record);
    record->clear(texture);
    EXPECT_EQ(record->submit().wait(5s), wait_result_e::ready);
    EXPECT_EQ(device.validation_errors(), 0U);
}
#endif

}} // namespace miximus::gpu
