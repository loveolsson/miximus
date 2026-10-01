#include "gpu/detail/device.hpp"
#include "gpu/detail/fatal.hpp"
#include "gpu/detail/recording.hpp"
#include "gpu/detail/resource.hpp"
#include "utils/shutdown_watchdog.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <stdexcept>

namespace miximus::gpu::detail { namespace {

TEST(GpuFailure, DeviceLossExitsWithDiagnostic)
{
    EXPECT_EXIT(
        {
            utils::start_shutdown_watchdog(std::chrono::seconds(1));
            check(VK_ERROR_DEVICE_LOST, "test submission");
        },
        testing::ExitedWithCode(EXIT_FAILURE),
        "test submission: Vulkan device lost");
}

TEST(GpuFailure, RecoverableApiRejectionStillThrows)
{
    EXPECT_THROW(check(VK_ERROR_FORMAT_NOT_SUPPORTED, "test format"), std::runtime_error);
}

TEST(GpuFailure, FatalExitDoesNotWaitForResourceDestruction)
{
    EXPECT_EXIT(
        {
            utils::start_shutdown_watchdog(std::chrono::seconds(1));
            struct resource_s
            {
                ~resource_s() { std::abort(); }
            } resource;
            fatal_gpu_error("test stalled transfer");
        },
        testing::ExitedWithCode(EXIT_FAILURE),
        "test stalled transfer");
}

}} // namespace miximus::gpu::detail
