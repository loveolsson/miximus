#pragma once

#include "conversion_texture.hpp"
#include "frame_staging.hpp"
#include "gpu/texture_frame.hpp"
#include "transfer_layout.hpp"

namespace miximus::gpu::transfer::detail {

// Destruction order is registration, frame, reservation, including failed allocation.
// Services release these on their resource worker after all leases and uses retire.
struct slot_resources_s
{
    memory_reservation_s             reservation;
    texture_frame_ptr                frame;
    std::unique_ptr<frame_staging_s> staging;

    void initialize(device_s&                      device,
                    recording_context_s&           recording,
                    const texture_transfer_plan_s& plan,
                    sampling_e                     sampling,
                    frame_staging_s::direction_e   direction,
                    conversion_texture_s&          conversion,
                    std::optional<sampling_e>      conversion_sampling,
                    memory_budget_s&               budget)
    {
        reservation = memory_reservation_s(budget, estimate_slot_memory_usage(plan, sampling));
        conversion.initialize(device, plan.host_layout.image_dimensions, conversion_sampling, budget);
        frame = std::make_shared<texture_frame_s>(device, plan.host_layout, sampling);
        frame->set_conversion_texture(conversion.texture);
        staging = std::make_unique<frame_staging_s>(device, plan, direction, frame.get(), &recording);
        reservation.resize(slot_memory_usage(plan, staging->allocation_bytes(), sampling));
    }

    void release()
    {
        staging.reset();
        frame.reset();
        reservation.reset();
    }
};
} // namespace miximus::gpu::transfer::detail
