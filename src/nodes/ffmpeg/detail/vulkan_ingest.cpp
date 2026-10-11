#include "vulkan_ingest.hpp"

#include "gpu/detail/external_buffer.hpp"
#include "gpu/detail/external_memory.hpp"
#include "gpu/detail/fatal.hpp"

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <thread>
#include <utility>

namespace miximus::nodes::ffmpeg::detail {

namespace {

VkResult submit_decode_copy(AVHWDeviceContext*     device,
                            const VolkDeviceTable& dispatch,
                            VkQueue                queue,
                            uint32_t               family,
                            const VkSubmitInfo2&   submit,
                            VkFence                fence)
{
    auto* native = static_cast<AVVulkanDeviceContext*>(device->hwctx);
    if ((native->queue_flags & VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR) != 0U) {
        return dispatch.vkQueueSubmit2(queue, 1, &submit, fence);
    }

#if FF_API_VULKAN_SYNC_QUEUES
    // FFmpeg retains these callbacks for drivers without internally synchronized
    // queues. Keep the compatibility API isolated; removing the locks races the
    // decoder's submissions on the same queue.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    native->lock_queue(device, family, 0);
    const auto result = dispatch.vkQueueSubmit2(queue, 1, &submit, fence);
    native->unlock_queue(device, family, 0);
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
    return result;
#else
    throw std::runtime_error("Decoder queue requires internal synchronization");
#endif
}

void check(VkResult result, const char* operation)
{
    if (result == VK_ERROR_DEVICE_LOST) {
        gpu::detail::fatal_gpu_error(operation, "FFmpeg Vulkan device lost");
    }

    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::format("{}: Vulkan error {}", operation, static_cast<int>(result)));
    }
}

struct frame_lock_s
{
    AVHWFramesContext*     frames;
    AVVulkanFramesContext* context;
    AVVkFrame*             frame;
    frame_lock_s(AVHWFramesContext* frame_context, AVVkFrame* image)
        : frames(frame_context)
        , context(static_cast<AVVulkanFramesContext*>(frame_context->hwctx))
        , frame(image)
    {
        context->lock_frame(frames, frame);
    }

    frame_lock_s(const frame_lock_s& other)            = delete;
    frame_lock_s& operator=(const frame_lock_s& other) = delete;
    frame_lock_s(frame_lock_s&& other)                 = delete;
    frame_lock_s& operator=(frame_lock_s&& other)      = delete;

    ~frame_lock_s() { context->unlock_frame(frames, frame); }
};

} // namespace

struct vulkan_ingest_s::impl_s
{
    gpu::device_s&                                  device;
    gpu::recording_context_s                        recordings;
    AVBufferRef*                                    device_ref{};
    AVHWDeviceContext*                              av_device{};
    AVVulkanDeviceContext*                          native{};
    VolkDeviceTable                                 vk{};
    VolkInstanceTable                               instance{};
    VkBuffer                                        buffer{};
    VkDeviceMemory                                  memory{};
    VkCommandPool                                   pool{};
    VkCommandBuffer                                 command{};
    VkFence                                         fence{};
    VkQueue                                         queue{};
    uint32_t                                        family{};
    size_t                                          bytes{};
    bool                                            used{};
    std::unique_ptr<gpu::detail::external_buffer_s> imported;

    explicit impl_s(gpu::device_s& gpu)
        : device(gpu)
        , recordings(gpu.create_recording_context(2))
    {
    }

    impl_s(const impl_s& other)            = delete;
    impl_s& operator=(const impl_s& other) = delete;
    impl_s(impl_s&& other)                 = delete;
    impl_s& operator=(impl_s&& other)      = delete;

    ~impl_s() { reset(); }

    void reset()
    {
        imported.reset();
        if (native != nullptr) {
            if (fence != nullptr) {
                vk.vkDestroyFence(native->act_dev, fence, native->alloc);
            }

            if (pool != nullptr) {
                vk.vkDestroyCommandPool(native->act_dev, pool, native->alloc);
            }

            if (buffer != nullptr) {
                vk.vkDestroyBuffer(native->act_dev, buffer, native->alloc);
            }

            if (memory != nullptr) {
                vk.vkFreeMemory(native->act_dev, memory, native->alloc);
            }
        }

        fence   = {};
        pool    = {};
        command = {};
        buffer  = {};
        memory  = {};
        av_buffer_unref(&device_ref);
        av_device = nullptr;
        native    = nullptr;
        used      = false;
        bytes     = 0;
    }

    void initialize(AVHWFramesContext* frames, size_t size, const AVVkFrame& frame)
    {
        auto* context = reinterpret_cast<AVHWDeviceContext*>(frames->device_ref->data);
        if (native != nullptr && av_device == context && bytes == size) {
            return;
        }

        reset();
        device_ref = av_buffer_ref(frames->device_ref);
        if (device_ref == nullptr) {
            throw std::bad_alloc{};
        }

        av_device = context;
        native    = static_cast<AVVulkanDeviceContext*>(context->hwctx);
        volkLoadDeviceTable(&vk, native->act_dev);
        volkLoadInstanceTable(&instance, native->inst);
        const bool external = std::any_of(
            native->enabled_dev_extensions,
            native->enabled_dev_extensions + native->nb_enabled_dev_extensions,
            [](const char* name) { return std::string_view(name) == gpu::detail::opaque_memory_extension; });
        if (!external) {
            throw std::runtime_error("Decoder cannot export opaque GPU memory");
        }

        VkPhysicalDeviceExternalBufferInfo capability{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                                      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                      .handleType = gpu::detail::opaque_memory_handle_type};
        VkExternalBufferProperties         supported{.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        instance.vkGetPhysicalDeviceExternalBufferProperties(native->phys_dev, &capability, &supported);
        if ((supported.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ==
            0U) {
            throw std::runtime_error("Decoder component buffers cannot be exported on this device");
        }

        family = UINT32_MAX;
        for (int i = 0; i < native->nb_qf; ++i) {
            const auto& candidate = std::span(native->qf)[i];
            if (candidate.num > 0 &&
                (candidate.flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0U &&
                (std::cmp_equal(frame.queue_family[0], VK_QUEUE_FAMILY_IGNORED) ||
                 std::cmp_equal(frame.queue_family[0], candidate.idx))) {
                family = candidate.idx;
                break;
            }
        }

        if (family == UINT32_MAX) {
            throw std::runtime_error("No compatible decoder transfer queue");
        }

        VkDeviceQueueInfo2 queue_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .flags = native->queue_flags, .queueFamilyIndex = family};
        vk.vkGetDeviceQueue2(native->act_dev, &queue_info, &queue);
        VkCommandPoolCreateInfo pool_info{.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .queueFamilyIndex = family};
        check(vk.vkCreateCommandPool(native->act_dev, &pool_info, native->alloc, &pool), "create video copy pool");
        VkCommandBufferAllocateInfo command_info{.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .commandPool        = pool,
                                                 .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                 .commandBufferCount = 1};
        check(vk.vkAllocateCommandBuffers(native->act_dev, &command_info, &command), "allocate video copy command");
        VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vk.vkCreateFence(native->act_dev, &fence_info, native->alloc, &fence), "create video copy fence");
        VkExternalMemoryBufferCreateInfo external_info{
            .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(gpu::detail::opaque_memory_handle_type)};
        VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .pNext = &external_info,
                                       .size  = size,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        check(vk.vkCreateBuffer(native->act_dev, &buffer_info, native->alloc, &buffer), "create GPU component buffer");
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(native->act_dev, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        instance.vkGetPhysicalDeviceMemoryProperties(native->phys_dev, &properties);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1U << i)) != 0U &&
                (std::span(properties.memoryTypes)[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                memory_type = i;
                break;
            }
        }

        if (memory_type == UINT32_MAX) {
            throw std::runtime_error("No device-local memory for video components");
        }

        VkMemoryDedicatedAllocateInfo dedicated{.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                .buffer = buffer};
        VkExportMemoryAllocateInfo    export_info{
            .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext       = &dedicated,
            .handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(gpu::detail::opaque_memory_handle_type)};
        VkMemoryAllocateInfo allocation{.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .pNext           = &export_info,
                                        .allocationSize  = requirements.size,
                                        .memoryTypeIndex = memory_type};
        check(vk.vkAllocateMemory(native->act_dev, &allocation, native->alloc, &memory),
              "allocate GPU component buffer");
        check(vk.vkBindBufferMemory(native->act_dev, buffer, memory, 0), "bind GPU component buffer");
        auto                         handle = gpu::detail::export_opaque_memory(vk, native->act_dev, memory);
        VkPhysicalDeviceIDProperties identity{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2  properties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                 .pNext = &identity};
        instance.vkGetPhysicalDeviceProperties2(native->phys_dev, &properties2);
        gpu::detail::external_buffer_description_s description{.handle = handle.get(),
                                                               .bytes  = size,
                                                               .allocation_bytes =
                                                                   static_cast<size_t>(requirements.size),
                                                               .memory_type = memory_type};
        std::memcpy(description.device_uuid.data(), identity.deviceUUID, VK_UUID_SIZE);
        std::memcpy(description.driver_uuid.data(), identity.driverUUID, VK_UUID_SIZE);
        imported = std::make_unique<gpu::detail::external_buffer_s>(device, description);
        bytes    = size;
    }

    void copy(decoded_frame_s& decoded)
    {
        auto* frame  = decoded.native_frame();
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (static_cast<AVVulkanFramesContext*>(frames->hwctx)->nb_layers > 1) {
            throw std::runtime_error("Layered Vulkan decoder surfaces are unsupported by this GPU ingress");
        }

        auto*       image  = reinterpret_cast<AVVkFrame*>(frame->data[0]);
        const auto* format = av_pix_fmt_desc_get(frames->sw_format);
        const int   planes = av_pix_fmt_count_planes(frames->sw_format);
        const int   images = std::span(image->img)[1] != nullptr ? planes : 1;
        initialize(frames, decoded.buffer_bytes, *image);
        check(vk.vkResetCommandPool(native->act_dev, pool, 0), "reset video copy pool");
        check(vk.vkResetFences(native->act_dev, 1, &fence), "reset video copy fence");
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vk.vkBeginCommandBuffer(command, &begin), "begin video copy");
        {
            frame_lock_s                       lock(frames, image);
            std::vector<VkImageMemoryBarrier2> barriers;
            std::vector<VkSemaphoreSubmitInfo> waits;
            std::vector<VkSemaphoreSubmitInfo> signals;
            for (int i = 0; i < images; ++i) {
                if (std::span(image->queue_family)[i] != VK_QUEUE_FAMILY_IGNORED &&
                    std::span(image->queue_family)[i] != family) {
                    throw std::runtime_error("Decoder image needs unsupported queue ownership transfer");
                }

                barriers.push_back({
                    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask       = std::span(image->access)[i],
                    .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout           = std::span(image->layout)[i],
                    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image               = std::span(image->img)[i],
                    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}
                });
                waits.push_back({.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                 .semaphore = std::span(image->sem)[i],
                                 .value     = std::span(image->sem_value)[i],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
                signals.push_back({.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = std::span(image->sem)[i],
                                   .value     = std::span(image->sem_value)[i] + 1,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
            }

            VkBufferMemoryBarrier2 buffer_barrier{.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                  .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                  .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                                                  .dstQueueFamilyIndex = family,
                                                  .buffer              = buffer,
                                                  .size                = VK_WHOLE_SIZE};
            VkDependencyInfo       dependency{.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .bufferMemoryBarrierCount = used ? 1U : 0U,
                                              .pBufferMemoryBarriers    = &buffer_barrier,
                                              .imageMemoryBarrierCount  = static_cast<uint32_t>(barriers.size()),
                                              .pImageMemoryBarriers     = barriers.data()};
            vk.vkCmdPipelineBarrier2(command, &dependency);
            const auto& header = decoded.component_header();
            vk.vkCmdUpdateBuffer(command, buffer, 0, sizeof(header), &header);
            for (int plane = 0; plane < planes; ++plane) {
                unsigned component = 0;
                while (component < format->nb_components && std::span(format->comp)[component].plane != plane) {
                    ++component;
                }

                if (component == format->nb_components) {
                    throw std::runtime_error("Unknown decoded plane");
                }

                const auto&       layout = header.components.at(component);
                const auto        sx     = (layout.encoding >> 16) & 15U;
                const auto        sy     = (layout.encoding >> 20) & 15U;
                VkBufferImageCopy region{
                    .bufferOffset     = layout.offset - std::span(format->comp)[component].offset,
                    .imageSubresource = {.aspectMask = images > 1 || planes == 1
                                                           ? VK_IMAGE_ASPECT_COLOR_BIT
                                                           : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_PLANE_0_BIT
                                                                                             << plane),
                                         .layerCount = 1},
                    .imageExtent      = {static_cast<uint32_t>((frame->width + (1 << sx) - 1) >> sx),
                                         static_cast<uint32_t>((frame->height + (1 << sy) - 1) >> sy),
                                         1}
                };

                vk.vkCmdCopyImageToBuffer(command,
                                          std::span(image->img)[images == 1 ? 0 : plane],
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          buffer,
                                          1,
                                          &region);
            }

            buffer_barrier.srcStageMask         = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            buffer_barrier.srcAccessMask        = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            buffer_barrier.dstStageMask         = VK_PIPELINE_STAGE_2_NONE;
            buffer_barrier.dstAccessMask        = VK_ACCESS_2_NONE;
            buffer_barrier.srcQueueFamilyIndex  = family;
            buffer_barrier.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_EXTERNAL;
            dependency.imageMemoryBarrierCount  = 0;
            dependency.bufferMemoryBarrierCount = 1;
            vk.vkCmdPipelineBarrier2(command, &dependency);
            check(vk.vkEndCommandBuffer(command), "end video copy");
            VkCommandBufferSubmitInfo commands{.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                               .commandBuffer = command};
            VkSubmitInfo2             submit{.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                             .waitSemaphoreInfoCount   = static_cast<uint32_t>(waits.size()),
                                             .pWaitSemaphoreInfos      = waits.data(),
                                             .commandBufferInfoCount   = 1,
                                             .pCommandBufferInfos      = &commands,
                                             .signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()),
                                             .pSignalSemaphoreInfos    = signals.data()};
            check(submit_decode_copy(av_device, vk, queue, family, submit, fence), "submit video component copy");
            for (int i = 0; i < images; ++i) {
                std::span(image->layout)[i] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                std::span(image->access)[i] = VK_ACCESS_2_TRANSFER_READ_BIT;
                ++std::span(image->sem_value)[i];
            }
        }

        const auto done = vk.vkWaitForFences(native->act_dev, 1, &fence, VK_TRUE, 30'000'000'000ULL);
        if (done != VK_SUCCESS) {
            gpu::detail::fatal_gpu_error("Decoded GPU copy did not complete");
        }
    }
};

vulkan_ingest_s::vulkan_ingest_s(gpu::device_s& device)
    : impl_(std::make_unique<impl_s>(device))
{
}

vulkan_ingest_s::~vulkan_ingest_s() = default;

void vulkan_ingest_s::convert(decoded_frame_s& frame, const gpu::texture_s& target)
{
    impl_->copy(frame);
    auto record = impl_->recordings.try_record();
    if (!record) {
        throw std::runtime_error("Video conversion recording unavailable");
    }

    const auto complete = impl_->imported->convert(*record, target, frame.color);
    if (complete.wait(std::chrono::seconds(30)) != gpu::wait_result_e::ready) {
        gpu::detail::fatal_gpu_error("Decoded GPU conversion did not complete");
    }

    impl_->used = true;
}

} // namespace miximus::nodes::ffmpeg::detail
