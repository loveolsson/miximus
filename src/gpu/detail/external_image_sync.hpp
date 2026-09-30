#pragma once

#include "external_image.hpp"

#include <chrono>
#include <volk.h>

namespace miximus::gpu::detail {

struct resource_state_s;

struct external_image_read_s
{
    std::shared_ptr<resource_state_s> lifetime;
    VkSemaphore                       semaphore{};
};

extern const uint32_t external_image_queue_family;

external_image_read_s prepare_external_image_read(const std::shared_ptr<device_state_s>& device,
                                                  const external_image_s&                descriptor,
                                                  std::chrono::milliseconds              timeout);

} // namespace miximus::gpu::detail
