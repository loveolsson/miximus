#include "external_image_export_state.hpp"
#include "external_image_win.hpp"

namespace miximus::gpu::detail {

void external_image_export_s::state_s::initialize(const std::shared_ptr<device_state_s>& device, extent_s extent)
{
    exported   = create_external_image(device, extent);
    descriptor = {.handle = exported->external_memory_handle.get(), .extent = extent};
}

} // namespace miximus::gpu::detail
