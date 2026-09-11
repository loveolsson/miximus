#include "frame_staging.hpp"

#include "logger/logger.hpp"

namespace miximus::gpu::transfer::detail {

frame_staging_s::frame_staging_s(device_s&                      device,
                                 const texture_transfer_plan_s& plan,
                                 direction_e                    direction,
                                 texture_frame_s*               frame,
                                 recording_context_s*           recording_context)
    : backend_(create_transfer_backend(device, plan, direction, *frame, recording_context))
    , direction_(direction)
{
    const auto log = getlog("gpu");
    if (log->should_log(spdlog::level::debug)) {
        log->debug("Transfer selected: backend={} direction={} size={}x{} format={} stride={} policy={}",
                   backend_name(),
                   direction == direction_e::cpu_to_gpu ? "upload" : "readback",
                   plan.host_layout.image_dimensions.x,
                   plan.host_layout.image_dimensions.y,
                   static_cast<int>(plan.host_layout.pixel_format),
                   plan.host_layout.row_stride_bytes,
                   device.uses_cuda_transfers() ? "cuda" : "vulkan");
    }
}

bool frame_staging_s::transfer_ready()
{
    if (!backend_->transfer_ready()) {
        return false;
    }

    if (!reported_completion_) {
        const auto log = getlog("gpu");
        if (log->should_log(spdlog::level::debug)) {
            log->debug("Transfer completed: backend={} direction={} bytes={}",
                       backend_name(),
                       direction_ == direction_e::cpu_to_gpu ? "upload" : "readback",
                       host_buffer_size_bytes());
        }

        reported_completion_ = true;
    }

    return true;
}
} // namespace miximus::gpu::transfer::detail
