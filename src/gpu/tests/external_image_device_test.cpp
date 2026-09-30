#include "gpu/detail/external_image_support.hpp"
#include "gpu/device.hpp"
#include "logger/logger.hpp"

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
    const auto before = baseline.diagnostics();
    EXPECT_EQ(baseline.external_image_import_support(), external_image_import_support_e::not_requested);

    options.external_image_import = true;
    device_s   importing(options);
    const auto after = importing.diagnostics();
    EXPECT_EQ(before.selected_uuid, after.selected_uuid);
    EXPECT_EQ(before.separate_present_queue, after.separate_present_queue);
    EXPECT_EQ(before.swapchain_maintenance, after.swapchain_maintenance);
    EXPECT_EQ(before.present_wait, after.present_wait);
    EXPECT_FALSE(importing.uses_cuda_transfers());
    const auto support = importing.external_image_import_support();
    EXPECT_NE(support, external_image_import_support_e::not_requested);

    std::vector<std::string_view> available;
    const auto&                   candidates = after.devices;
    for (const auto& candidate : candidates) {
        if (candidate.uuid == after.selected_uuid) {
            for (const auto& extension : candidate.extensions) {
                available.emplace_back(extension);
            }
        }
    }
    EXPECT_EQ(support, detail::probe_external_image_import(true, available));
    const bool  enabled    = support == external_image_import_support_e::supported;
    const auto& diagnostic = after.external_image_import;
    EXPECT_EQ(diagnostic.enabled, enabled);
    EXPECT_EQ(diagnostic.enabled_extensions.size(), enabled ? detail::external_image_import_extensions().size() : 0U);
    EXPECT_EQ(diagnostic.missing_support.empty(), enabled);

    auto texture = importing.create_texture({.width = 32, .height = 32});
    auto context = importing.create_recording_context();
    auto record  = context.try_record();
    ASSERT_TRUE(record);
    record->clear(texture);
    EXPECT_EQ(record->submit().wait(5s), wait_result_e::ready);
    EXPECT_EQ(importing.validation_errors(), 0U);
    EXPECT_EQ(baseline.validation_errors(), 0U);
}

TEST_F(external_image_device, ImportAndCudaRequestsCanCoexist)
{
    auto options                  = test_options();
    options.use_cuda              = true;
    options.external_image_import = true;
    // CUDA qualification may legitimately fail on this machine/build. Import
    // support must remain independent, with no duplicate device extensions.
    device_s device(options);
    EXPECT_NE(device.external_image_import_support(), external_image_import_support_e::not_requested);
    auto texture = device.create_texture({.width = 32, .height = 32});
    auto record  = device.try_record();
    ASSERT_TRUE(record);
    record->clear(texture);
    EXPECT_EQ(record->submit().wait(5s), wait_result_e::ready);
    EXPECT_EQ(device.validation_errors(), 0U);
}

}} // namespace miximus::gpu
