#include "presenter.hpp"

#include "detail/device.hpp"
#include "logger/logger.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <optional>
#include <stdexcept>
#include <thread>

namespace miximus::gpu::detail {

struct presenter_state_s
{
    // Preserve the old fence-wait allowance; ordinary waits remain cancellable.
    // Application shutdown still has its independent progress watchdog.
    static constexpr auto retirement_timeout     = std::chrono::hours(1);
    static constexpr auto acquisition_timeout    = std::chrono::milliseconds(1);
    static constexpr auto retirement_timeout_ns  = std::chrono::nanoseconds(retirement_timeout).count();
    static constexpr auto acquisition_timeout_ns = std::chrono::nanoseconds(acquisition_timeout).count();

    struct swapchain_image_s
    {
        VkImage     image{};
        VkSemaphore present_ready{};
        VkFence     present_finished{};
        bool        presented{};
        bool        initialized{};
    };

    // Native surface/swapchain state belongs to the presentation worker after construction.
    std::shared_ptr<device_state_s> owner;

    VkSurfaceKHR                   surface{};
    VkSwapchainKHR                 swapchain{};
    VkFormat                       format{};
    uint64_t                       present_id{};
    presentation_pacing_e          pacing{presentation_pacing_e::display};
    extent_s                       extent{};
    std::vector<swapchain_image_s> images;

    VkSemaphore             image_acquired_semaphore{};
    completion_s            last_copy_completion;
    recording_context_s     recording_context;
    std::optional<uint32_t> acquired_index;
    bool                    acquire_wait_pending{};

    // Publication, resize requests and metrics cross threads under this mutex.
    mutable std::mutex          mutex;
    extent_s                    requested_extent{};
    texture_s                   pending_image;
    completion_s                pending_ready;
    std::shared_ptr<const void> pending_lease;
    std::shared_ptr<const void> last_copy_lease;
    presentation_metrics_s      counters;

    // The source callback and publication mailbox are alternative producer contracts.
    presentation_source_s source;
    std::jthread          worker;

    presenter_state_s(device_s& gpu, GLFWwindow* window, extent_s initial_extent, presentation_source_s frame_source)
        : owner(gpu.state_)
        , recording_context(gpu.create_recording_context())
        , requested_extent(initial_extent)
        , source(std::move(frame_source))
    {
        if (!owner->options.presentation) {
            throw std::invalid_argument("device was created without presentation support");
        }

        if (!owner->swapchain_maintenance) {
            throw std::runtime_error("presenter requires swapchain maintenance for finite WSI retirement");
        }

        check(glfwCreateWindowSurface(owner->instance, window, nullptr, &surface), "GLFW surface");
        try {
            VkBool32 supported{};
            check(owner->instance_vk.vkGetPhysicalDeviceSurfaceSupportKHR(
                      owner->physical, owner->queue_family, surface, &supported),
                  "surface support");
            if (supported == 0U) {
                throw std::runtime_error("selected device cannot present to this surface");
            }

            // FIFO keeps complete frames aligned to the display. Where present-wait
            // is available, keep only one outstanding presentation and prepare the
            // following frame immediately after the previous display boundary.
            pacing = owner->present_wait ? presentation_pacing_e::display : presentation_pacing_e::application;

            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            check(owner->vk.vkCreateSemaphore(owner->device, &info, nullptr, &image_acquired_semaphore),
                  "acquire semaphore");
            worker = std::jthread([this](const std::stop_token& stop) { run(stop); });
        } catch (...) {
            if (image_acquired_semaphore != nullptr) {
                owner->vk.vkDestroySemaphore(owner->device, image_acquired_semaphore, nullptr);
            }

            owner->instance_vk.vkDestroySurfaceKHR(owner->instance, surface, nullptr);
            throw;
        }
    }

    presenter_state_s(const presenter_state_s&)            = delete;
    presenter_state_s& operator=(const presenter_state_s&) = delete;
    presenter_state_s(presenter_state_s&&)                 = delete;
    presenter_state_s& operator=(presenter_state_s&&)      = delete;

    std::unique_ptr<recording_s> try_record() { return recording_context.try_record(); }

    void publish(texture_s image, completion_s ready, std::shared_ptr<const void> lease)
    {
        if (source.next_frame) {
            throw std::logic_error("cannot publish to a presenter with a frame source");
        }

        validate_frame(image, ready);
        const std::scoped_lock guard(mutex);
        if (pending_image) {
            ++counters.mailbox_drops;
        }

        pending_image = std::move(image);
        pending_ready = std::move(ready);
        pending_lease = std::move(lease);
    }

    void validate_frame(const texture_s& image, const completion_s& ready)
    {
        if (!image || !ready || image.state_->owner != owner || ready.state_ != owner) {
            throw std::invalid_argument("presentation needs an image and submitted dependency on this device");
        }

        if (image.format() == format_e::r32_uint) {
            throw std::invalid_argument("cannot present packed integer storage");
        }

        VkFormatProperties properties{};
        owner->instance_vk.vkGetPhysicalDeviceFormatProperties(
            owner->physical, native_format(image.format()), &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0) {
            throw std::runtime_error("image format cannot be blitted for presentation");
        }
    }

    bool retire_swapchain()
    {
        if (last_copy_completion && last_copy_completion.wait(retirement_timeout) != wait_result_e::ready) {
            return false;
        }

        if (const auto index = acquired_index) {
            if (acquire_wait_pending) {
                auto       record   = try_record();
                const auto deadline = std::chrono::steady_clock::now() + retirement_timeout;
                while (!record && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    record = try_record();
                }

                if (!record) {
                    return false;
                }

                VkSemaphoreSubmitInfo wait{};
                wait.sType           = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                wait.semaphore       = image_acquired_semaphore;
                wait.stageMask       = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                last_copy_completion = enqueue_recording(record->state_, std::span(&wait, 1));
                record.reset();
                if (last_copy_completion.wait(retirement_timeout) != wait_result_e::ready) {
                    return false;
                }

                acquire_wait_pending = false;
            }

            VkReleaseSwapchainImagesInfoEXT release{};
            release.sType           = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT;
            release.swapchain       = swapchain;
            release.imageIndexCount = 1;
            release.pImageIndices   = &*index;
            if (owner->vk.vkReleaseSwapchainImagesEXT(owner->device, &release) != VK_SUCCESS) {
                return false;
            }

            acquired_index.reset();
        }

        // GPU copy completion does not prove that WSI consumed its present semaphore.
        // The maintenance fence closes that separate lifetime before destruction.
        for (auto& image : images) {
            if (image.presented &&
                owner->vk.vkWaitForFences(owner->device, 1, &image.present_finished, VK_TRUE, retirement_timeout_ns) !=
                    VK_SUCCESS) {
                return false;
            }
        }

        for (auto& image : images) {
            owner->vk.vkDestroyFence(owner->device, image.present_finished, nullptr);
            owner->vk.vkDestroySemaphore(owner->device, image.present_ready, nullptr);
        }

        images.clear();
        if (swapchain != nullptr) {
            owner->vk.vkDestroySwapchainKHR(owner->device, swapchain, nullptr);
        }

        swapchain = VK_NULL_HANDLE;
        return true;
    }

    ~presenter_state_s()
    {
        worker.request_stop();
        if (worker.joinable()) {
            worker.join();
        }

        last_copy_lease.reset();
        pending_lease.reset();
        if (image_acquired_semaphore != nullptr) {
            owner->vk.vkDestroySemaphore(owner->device, image_acquired_semaphore, nullptr);
        }

        if (surface != nullptr) {
            owner->instance_vk.vkDestroySurfaceKHR(owner->instance, surface, nullptr);
        }
    }

    void recreate(extent_s requested)
    {
        if (!retire_swapchain()) {
            throw std::runtime_error("timed out retiring old swapchain");
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        check(owner->instance_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(owner->physical, surface, &capabilities),
              "surface capabilities");
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
            throw std::runtime_error("surface does not support transfer presentation");
        }

        uint32_t count{};
        check(owner->instance_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(owner->physical, surface, &count, nullptr),
              "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(owner->instance_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(owner->physical, surface, &count, formats.data()),
              "surface formats");
        auto selected = std::ranges::find_if(formats, [](const auto& candidate) {
            return (candidate.format == VK_FORMAT_B8G8R8A8_SRGB || candidate.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                   candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (selected == formats.end()) {
            throw std::runtime_error("surface has no supported sRGB presentation format");
        }

        format = selected->format;

        VkFormatProperties format_properties{};
        owner->instance_vk.vkGetPhysicalDeviceFormatProperties(owner->physical, format, &format_properties);
        if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
            throw std::runtime_error("presentation format cannot receive blits");
        }

        extent = requested;
        if (capabilities.currentExtent.width != UINT32_MAX) {
            extent = {.width = capabilities.currentExtent.width, .height = capabilities.currentExtent.height};
        } else {
            extent.width =
                std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height =
                std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }

        VkSwapchainCreateInfoKHR info{};
        info.sType         = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface       = surface;
        info.minImageCount = std::max(3U, capabilities.minImageCount);
        if (capabilities.maxImageCount != 0U) {
            info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
        }

        info.imageFormat      = format;
        info.imageColorSpace  = selected->colorSpace;
        info.imageExtent      = {.width = extent.width, .height = extent.height};
        info.imageArrayLayers = 1;
        info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.preTransform     = capabilities.currentTransform;
        if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0U) {
            throw std::runtime_error("Screen surface does not support required opaque presentation");
        }
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode    = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped        = VK_TRUE;
        check(owner->vk.vkCreateSwapchainKHR(owner->device, &info, nullptr, &swapchain), "swapchain creation");
        check(owner->vk.vkGetSwapchainImagesKHR(owner->device, swapchain, &count, nullptr), "swapchain images");
        std::vector<VkImage> handles(count);
        check(owner->vk.vkGetSwapchainImagesKHR(owner->device, swapchain, &count, handles.data()), "swapchain images");
        images.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            images[i].image = handles[i];

            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            check(owner->vk.vkCreateSemaphore(owner->device, &semaphore, nullptr, &images[i].present_ready),
                  "present semaphore");

            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            check(owner->vk.vkCreateFence(owner->device, &fence, nullptr, &images[i].present_finished),
                  "present fence");
        }

        const std::scoped_lock guard(mutex);
        ++counters.recreations;
    }

    bool present_frame(const texture_s&                   current,
                       const completion_s&                ready,
                       const std::shared_ptr<const void>& current_lease,
                       uint32_t                           index)
    {
        auto& image = images[index];
        // An acquired image must consume its semaphore even when stop
        // arrives. Finish this bounded transaction before leaving.
        auto       record   = try_record();
        const auto deadline = std::chrono::steady_clock::now() + retirement_timeout;
        while (!record && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            record = try_record();
        }

        if (!record) {
            throw std::runtime_error("presentation command arenas remained busy");
        }

        auto& recording = *record->state_;
        recording.transition(current.state_,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_BLIT_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT);

        VkImageMemoryBarrier2 barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout           = image.initialized ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image.image;
        barrier.subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel   = 0,
                                       .levelCount     = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount     = 1};

        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &barrier;
        const auto command_buffer          = recording.arena->commands;
        owner->vk.vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkImageBlit blit{};
        blit.srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        blit.srcOffsets[1]  = {.x = static_cast<int32_t>(current.extent().width),
                               .y = static_cast<int32_t>(current.extent().height),
                               .z = 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1]  = {
             .x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
        owner->vk.vkCmdBlitImage(command_buffer,
                                 current.state_->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 image.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1,
                                 &blit,
                                 VK_FILTER_LINEAR);
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        owner->vk.vkCmdPipelineBarrier2(command_buffer, &dependency);

        std::array<VkSemaphoreSubmitInfo, 2> waits{};
        waits[0].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[0].semaphore = image_acquired_semaphore;
        waits[0].stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;

        // Publication guarantees native submission, not GPU completion. Carry
        // the producer's timeline dependency into the consumer GPU submission.
        waits[1].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[1].semaphore = owner->timeline;
        waits[1].value     = ready.submission_->value.load(std::memory_order_acquire);
        waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSemaphoreSubmitInfo signal{};
        signal.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore     = image.present_ready;
        signal.stageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        last_copy_completion = enqueue_recording(record->state_, waits, std::span(&signal, 1));
        last_copy_lease      = current_lease;
        record.reset();
        acquire_wait_pending = false;
        image.initialized    = true;

        ++present_id;
        VkPresentIdKHR identity{};
        identity.sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        identity.swapchainCount = 1;
        identity.pPresentIds    = &present_id;

        VkSwapchainPresentFenceInfoEXT fence{};
        fence.sType          = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        fence.pNext          = owner->present_wait ? &identity : nullptr;
        fence.swapchainCount = 1;
        fence.pFences        = &image.present_finished;

        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.pNext              = &fence;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &image.present_ready;
        present.swapchainCount     = 1;
        present.pSwapchains        = &swapchain;
        present.pImageIndices      = &index;
        const auto start           = std::chrono::steady_clock::now();

        // A binary WSI wait requires its signal submission to have been accepted.
        // This wait belongs to the presenter, never to the graph render thread.
        if (last_copy_completion.wait_submitted(retirement_timeout) != wait_result_e::ready) {
            throw std::runtime_error("presentation copy submission timed out");
        }
        VkResult presented{};
        {
            const std::scoped_lock queue_lock(owner->separate_present_queue ? owner->present_mutex
                                                                            : owner->queue_mutex);
            presented = owner->vk.vkQueuePresentKHR(owner->present_queue, &present);
        }

        image.presented = true;
        acquired_index.reset();
        const bool rebuild_required = presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR;
        if (!rebuild_required) {
            check(presented, "queue present");
        }

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        {
            const std::scoped_lock guard(mutex);
            ++counters.presents;
            counters.present_call_max_us = std::max(counters.present_call_max_us, static_cast<uint64_t>(elapsed));
        }

        return rebuild_required;
    }

    std::optional<uint32_t> acquire_next_image(bool& rebuild)
    {
        // Reuse the acquire semaphore only after the previous GPU wait.
        if (last_copy_completion && !last_copy_completion.ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }

        last_copy_lease.reset();
        uint32_t   index{};
        const auto result = owner->vk.vkAcquireNextImageKHR(
            owner->device, swapchain, acquisition_timeout_ns, image_acquired_semaphore, VK_NULL_HANDLE, &index);
        if (result == VK_NOT_READY || result == VK_TIMEOUT) {
            {
                const std::scoped_lock guard(mutex);
                ++counters.acquire_misses;
            }

            return std::nullopt;
        }

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            rebuild = true;
            return std::nullopt;
        }

        if (result != VK_SUBOPTIMAL_KHR) {
            check(result, "acquire swapchain image");
        }

        rebuild              = result == VK_SUBOPTIMAL_KHR;
        acquired_index       = index;
        acquire_wait_pending = true;
        auto& image          = images[index];
        if (image.presented) {
            check(owner->vk.vkWaitForFences(owner->device, 1, &image.present_finished, VK_TRUE, retirement_timeout_ns),
                  "presentation retirement");
            check(owner->vk.vkResetFences(owner->device, 1, &image.present_finished), "reset presentation fence");
            image.presented = false;
        }

        return index;
    }

    std::optional<presentation_frame_s> next_source_frame(const std::stop_token& stop)
    {
        std::optional<presentation_frame_s> frame;
        // Acquisition supplies an available destination, not a display timestamp.
        // The source owns timestamp selection and any application pacing.
        while (!stop.stop_requested()) {
            frame = source.next_frame(pacing, stop);
            if (frame) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!frame) {
            return std::nullopt;
        }

        validate_frame(frame->image, frame->ready);
        return frame;
    }

    extent_s take_pending_frame(texture_s&                   current,
                                completion_s&                current_ready,
                                std::shared_ptr<const void>& current_lease,
                                bool&                        needs_present)
    {
        extent_s request{};
        {
            const std::scoped_lock guard(mutex);
            request = requested_extent;
            if (pending_image) {
                if (needs_present) {
                    ++counters.mailbox_drops;
                }

                needs_present = true;
                current       = std::move(pending_image);
                current_ready = std::move(pending_ready);
                current_lease = std::move(pending_lease);
                pending_image = {};
                pending_ready = {};
            }
        }

        return request;
    }

    bool complete_presentation(const std::stop_token& stop, bool& rebuild) const
    {
        if (owner->present_wait) {
            while (!stop.stop_requested()) {
                const auto result =
                    owner->vk.vkWaitForPresentKHR(owner->device, swapchain, present_id, acquisition_timeout_ns);
                if (result == VK_TIMEOUT) {
                    continue;
                }
                if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                    rebuild = true;
                    return true; // This frame has no usable display observation.
                }
                if (result == VK_SUBOPTIMAL_KHR) {
                    rebuild = true;
                } else {
                    check(result, "wait for presentation");
                }
                if (source.complete) {
                    source.complete({.time = utils::flicks_now(), .timing = presentation_timing_e::present_wait});
                }
                return true;
            }
            return false;
        }

        if (source.complete) {
            source.complete({.time = utils::flicks_now(), .timing = presentation_timing_e::submission_estimate});
        }
        return true;
    }

    static bool wait_for_producer_submission(const completion_s& ready, const std::stop_token& stop)
    {
        // Resolve a pending submission ticket without waiting for its GPU
        // work. Screen-node frames have already passed this boundary.
        const auto submitted = ready.wait_submitted(retirement_timeout, stop);
        if (submitted == wait_result_e::cancelled) {
            return false;
        }
        if (submitted != wait_result_e::ready) {
            throw std::runtime_error("presentation producer submission timed out");
        }
        return true;
    }

    void run_frames(const std::stop_token& stop)
    {
        texture_s                   current;
        completion_s                current_ready;
        std::shared_ptr<const void> current_lease;
        bool                        rebuild = true;
        bool                        needs_present{};
        extent_s                    previous_request{};
        while (!stop.stop_requested()) {
            const auto request = take_pending_frame(current, current_ready, current_lease, needs_present);

            // The producer owns frame cadence, including intentional
            // repeats. FIFO backpressure must not become a second producer
            // that inserts stale frames between scheduled publications.
            // A retained image is redrawn only for a swapchain change.
            const bool surface_hidden           = request.width == 0 || request.height == 0;
            const bool retained_frame_unchanged = !needs_present && !rebuild && request == previous_request;
            const bool mailbox_waiting          = !source.next_frame && (!current || retained_frame_unchanged);
            if (surface_hidden || mailbox_waiting) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (rebuild || request != previous_request) {
                recreate(request);
                previous_request = request;
                rebuild          = false;
            }

            const auto acquired = acquire_next_image(rebuild);
            if (!acquired) {
                continue;
            }
            const auto index = *acquired;

            if (source.next_frame) {
                auto frame = next_source_frame(stop);
                if (!frame) {
                    break; // Retirement consumes the outstanding acquire semaphore.
                }

                current       = std::move(frame->image);
                current_ready = std::move(frame->ready);
                current_lease = std::move(frame->lease);
            }

            if (!wait_for_producer_submission(current_ready, stop)) {
                break;
            }

            rebuild       = present_frame(current, current_ready, current_lease, index) || rebuild;
            needs_present = false;
            if (!rebuild && !complete_presentation(stop, rebuild)) {
                break;
            }
        }
    }

    void run(const std::stop_token& stop) noexcept
    {
        try {
            run_frames(stop);
        } catch (const std::exception& error) {
            const std::scoped_lock guard(mutex);
            counters.failure = error.what();
            logger::log_error_noexcept("gpu", "Vulkan presenter stopped: {}", error.what());
        }

        // The render thread polls stopped() before replacing a presenter. Finish
        // GPU/WSI retirement here so that replacement cannot wait on those uses.
        try {
            if (!retire_swapchain()) {
                throw std::runtime_error("timed out retiring Vulkan presentation resources");
            }
            last_copy_lease.reset();
        } catch (const std::exception& error) {
            logger::log_error_noexcept("gpu", "Vulkan presentation retirement failed: {}", error.what());
            std::terminate(); // Outstanding WSI uses cannot be destroyed safely.
        }

        const std::scoped_lock guard(mutex);
        counters.stopped = true;
    }
};
} // namespace miximus::gpu::detail

namespace miximus::gpu {

presenter_s::presenter_s(device_s& device, GLFWwindow* window, extent_s initial_extent, presentation_source_s source)
    : state_(std::make_unique<detail::presenter_state_s>(device, window, initial_extent, std::move(source)))
{
}

presenter_s::~presenter_s() = default;

void presenter_s::request_stop() noexcept { state_->worker.request_stop(); }
void presenter_s::publish(texture_s image, completion_s ready, std::shared_ptr<const void> lease)
{
    state_->publish(std::move(image), std::move(ready), std::move(lease));
}

void presenter_s::resize(extent_s extent)
{
    const std::scoped_lock guard(state_->mutex);
    state_->requested_extent = extent;
}

presentation_metrics_s presenter_s::metrics() const
{
    const std::scoped_lock guard(state_->mutex);
    return state_->counters;
}
} // namespace miximus::gpu
