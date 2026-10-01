#include "external_image_sync.hpp"

namespace miximus::gpu::detail {

const uint32_t external_image_queue_family = VK_QUEUE_FAMILY_EXTERNAL;

external_image_read_s prepare_external_image_read(const std::shared_ptr<device_state_s>& /* device */,
                                                  const external_image_s& /* descriptor */,
                                                  std::chrono::milliseconds /* timeout */)
{
    // The qualified CEF callback is delivered only after actual producer GPU
    // completion and retains the borrow until our GPU completion. NT handles
    // carry no readiness signal; importing one cannot establish this contract.
    return {};
}

} // namespace miximus::gpu::detail
