#pragma once

#include "gpu/device.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <volk.h>

namespace miximus::gpu::detail {

struct submission_engine_s
{
    device_state_s& owner;
    explicit submission_engine_s(device_state_s& device)
        : owner(device)
    {
    }
    void initialize();
    void start();
    void stop();
    // After device idle and resource retirement.
    void destroy_timeline();

  private:
    void run();

  public:
    // Only the submission worker calls vkQueueSubmit. Present shares its queue
    // mutex solely on devices without a second queue; recorders never take it.
    VkQueue    queue{};
    VkQueue    present_queue{};
    std::mutex queue_mutex;
    std::mutex present_mutex;

    VkSemaphore                                           timeline{};
    uint64_t                                              last_submitted_timeline_value{};
    std::atomic_uint64_t                                  completed_value{};
    std::mutex                                            contexts_mutex;
    std::vector<std::weak_ptr<recording_context_state_s>> contexts;
    std::atomic_bool                                      submission_failed{};
    std::thread                                           submission_worker;
    std::atomic_bool                                      stopping{};
    std::mutex                                            wake_mutex;
    std::condition_variable                               wake;
};
} // namespace miximus::gpu::detail
