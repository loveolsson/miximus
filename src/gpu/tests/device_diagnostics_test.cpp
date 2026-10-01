#include "gpu/device_diagnostics_json.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

namespace miximus::gpu { namespace {

TEST(device_diagnostics, PreservesAbsentFieldsAndEmptyArrays)
{
    const device_diagnostics_s report{.validation = true, .api_floor = "1.3"};
    const auto                 json = nlohmann::json::parse(format_device_diagnostics(report));

    EXPECT_TRUE(json.at("devices").is_array());
    EXPECT_TRUE(json.at("external_image_import").at("enabled_extensions").is_array());
    EXPECT_FALSE(json.contains("selected_uuid"));
    EXPECT_FALSE(json.contains("cuda_transfer_formats_qualified"));
    EXPECT_FALSE(json.contains("memory"));
}

TEST(device_diagnostics, PreservesFalseQualificationAndWideMemoryValues)
{
    device_diagnostics_s report;
    report.cuda_transfer_formats_qualified = false;
    report.memory = memory_diagnostics_s{.allocation_count = 2, .allocation_bytes = uint64_t{1} << 34};
    report.devices.push_back({.name = "test", .uuid = "abc"});
    report.devices.back().formats = {
        {.format = format_e::rgba_unorm8},
        {.format = format_e::rgba_unorm16},
        {.format = format_e::r32_uint},
    };
    const nlohmann::json json = report;

    EXPECT_EQ(json.at("cuda_transfer_formats_qualified"), false);
    EXPECT_EQ(json.at("memory").at("allocation_bytes"), uint64_t{1} << 34);
    EXPECT_TRUE(json.at("devices").at(0).at("queues").is_array());
    EXPECT_FALSE(json.at("devices").at(0).contains("portability_subset"));
    const auto& formats = json.at("devices").at(0).at("formats");
    EXPECT_EQ(formats.at(0).at("format"), "rgba_unorm8");
    EXPECT_EQ(formats.at(1).at("format"), "rgba_unorm16");
    EXPECT_EQ(formats.at(2).at("format"), "r32_uint");
}

}} // namespace miximus::gpu
