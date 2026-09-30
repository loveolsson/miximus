#include "device.hpp"
#include "external_image_sync.hpp"
#include "recording.hpp"
#include "resource.hpp"
#include "utils/owned_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace miximus::gpu::detail {
namespace {

struct read_fence_s : resource_state_s
{
    VkSemaphore semaphore{};

    read_fence_s()                                     = default;
    read_fence_s(const read_fence_s& other)            = delete;
    read_fence_s& operator=(const read_fence_s& other) = delete;
    read_fence_s(read_fence_s&& other)                 = delete;
    read_fence_s& operator=(read_fence_s&& other)      = delete;

    ~read_fence_s()
    {
        if (semaphore != VK_NULL_HANDLE) {
            owner->retire(last_use_timeline_value.load(),
                          [device = owner->device, destroy = owner->vk.vkDestroySemaphore, handle = semaphore] {
                              destroy(device, handle, nullptr);
                          });
        }
    }
};

std::shared_ptr<read_fence_s>
import_read_fence(const std::shared_ptr<device_state_s>& device, int dma_buf, std::chrono::milliseconds timeout)
{
    VkPhysicalDeviceExternalSemaphoreInfo query{
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };

    VkExternalSemaphoreProperties properties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };

    device->instance_vk.vkGetPhysicalDeviceExternalSemaphoreProperties(device->physical, &query, &properties);
    if ((properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
        throw std::runtime_error("DMA-BUF read requires importable sync-file semaphores");
    }

    auto fence   = std::make_shared<read_fence_s>();
    fence->owner = device;

    VkSemaphoreCreateInfo create{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    check(device->vk.vkCreateSemaphore(device->device, &create, nullptr, &fence->semaphore),
          "create DMA-BUF read semaphore");

    dma_buf_export_sync_file export_fence{};
    export_fence.flags = DMA_BUF_SYNC_READ;
    export_fence.fd    = -1;
    // No DMA_BUF_IOCTL_SYNC or CPU mapping: export the producer's write fences.
    // No fallback to an unsynchronized read if this contract is unavailable.
    if (ioctl(dma_buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_fence) < 0) {
        throw std::system_error(errno, std::generic_category(), "export DMA-BUF write fence");
    }

    // Snapshot the fence once and wait only on this private callback/worker.
    // Do not enqueue an unresolved foreign dependency that could stall the graph.
    utils::owned_fd_s sync_file{export_fence.fd};
    const auto        start = std::chrono::steady_clock::now();
    for (;;) {
        const auto elapsed   = std::chrono::steady_clock::now() - start;
        const auto remaining = std::max(std::chrono::milliseconds::zero(),
                                        timeout - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed));
        pollfd     descriptor{.fd = sync_file.get(), .events = POLLIN, .revents = 0};
        const int  result = poll(&descriptor, 1, static_cast<int>(std::min<int64_t>(remaining.count(), INT_MAX)));
        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result < 0) {
            throw std::system_error(errno, std::generic_category(), "wait for DMA-BUF write fence");
        }

        if (result == 0) {
            throw recording_unavailable_s("DMA-BUF producer readiness budget exhausted");
        }

        if ((descriptor.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0 || (descriptor.revents & POLLIN) == 0) {
            throw std::runtime_error("DMA-BUF producer fence failed");
        }

        break;
    }

    VkImportSemaphoreFdInfoKHR import{
        .sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore  = fence->semaphore,
        .flags      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd         = export_fence.fd,
    };
    const auto result = device->vk.vkImportSemaphoreFdKHR(device->device, &import);
    if (result != VK_SUCCESS) {
        check(result, "import DMA-BUF write fence");
    }

    (void)sync_file.release(); // Ownership transferred to Vulkan.
    return fence;
}

} // namespace

const uint32_t external_image_queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT;

external_image_read_s prepare_external_image_read(const std::shared_ptr<device_state_s>& device,
                                                  const external_image_s&                descriptor,
                                                  std::chrono::milliseconds              timeout)
{
    auto fence = import_read_fence(device, descriptor.handle, timeout);
    return {.lifetime = fence, .semaphore = fence->semaphore};
}

} // namespace miximus::gpu::detail
