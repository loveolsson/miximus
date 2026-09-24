#include "gpu/detail/device.hpp"
#include "gpu/detail/fatal.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <stdexcept>

namespace miximus::gpu::detail {

TEST(GpuFailure, DeviceLossExitsWithDiagnostic)
{
    EXPECT_EXIT(check(VK_ERROR_DEVICE_LOST, "test submission"),
                testing::ExitedWithCode(EXIT_FAILURE),
                "Fatal GPU error: test submission: Vulkan device lost");
}

TEST(GpuFailure, RecoverableApiRejectionStillThrows)
{
    EXPECT_THROW(check(VK_ERROR_FORMAT_NOT_SUPPORTED, "test format"), std::runtime_error);
}

TEST(GpuFailure, FatalExitDoesNotWaitForResourceDestruction)
{
    EXPECT_EXIT(
        {
            struct resource_s
            {
                ~resource_s() { std::abort(); }
            } resource;
            fatal_gpu_error("test stalled transfer");
        },
        testing::ExitedWithCode(EXIT_FAILURE),
        "Fatal GPU error: test stalled transfer");
}

} // namespace miximus::gpu::detail
