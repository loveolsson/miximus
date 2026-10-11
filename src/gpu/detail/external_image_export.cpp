#include "device.hpp"
#include "external_image_export_state.hpp"
#include "external_image_sync.hpp"
#include "recording.hpp"

namespace miximus::gpu::detail {

external_image_export_s::external_image_export_s(device_s& device, extent_s extent)
    : state_(std::make_shared<state_s>())
{
    state_->initialize(device.state_, extent);
}

external_image_export_s::~external_image_export_s() = default;

external_image_s external_image_export_s::descriptor() const { return state_->descriptor; }
size_t external_image_export_s::allocation_bytes() const { return state_->exported->external_allocation_bytes; }

void external_image_export_s::copy(recording_s& record, const texture_s& source, const draw_s& conversion)
{
    if (!record.state_ || record.state_->submission_attempted) {
        throw std::invalid_argument("External image export requires an active recording");
    }

    auto&      recording = *record.state_;
    const auto image     = state_->exported;
    if (recording.owner != image->owner || recording.layouts.contains(image.get())) {
        throw std::invalid_argument("External image export requires one write on its owning device");
    }

    const bool foreign = state_->foreign.load();
    recording.acquire_external_image(image,
                                     foreign ? external_image_queue_family : VK_QUEUE_FAMILY_IGNORED,
                                     foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                     {.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      .stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    record.draw(source, texture_s(image), conversion);
    recording.transition(
        image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);
    recording.release_external_image(image,
                                     external_image_queue_family,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     {.layout = VK_IMAGE_LAYOUT_GENERAL,
                                      .stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      .access = VK_ACCESS_2_MEMORY_WRITE_BIT});
    record.on_submitted([state = state_](const completion_s& /* completion */) { state->foreign.store(true); });
}

} // namespace miximus::gpu::detail
