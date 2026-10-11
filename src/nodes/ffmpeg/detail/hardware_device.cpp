#include "hardware_device.hpp"

#include "gpu/device_diagnostics.hpp"
#include "logger/logger.hpp"

#include <volk.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace miximus::nodes::ffmpeg::detail {

namespace {

struct device_owner_s
{
    AVBufferRef*             source{};
    std::vector<const char*> extensions;

    device_owner_s()                                       = default;
    device_owner_s(const device_owner_s& other)            = delete;
    device_owner_s& operator=(const device_owner_s& other) = delete;
    device_owner_s(device_owner_s&& other)                 = delete;
    device_owner_s& operator=(device_owner_s&& other)      = delete;

    ~device_owner_s() { av_buffer_unref(&source); }
};

bool matches_device(const AVVulkanDeviceContext& native, const std::string& expected_uuid)
{
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        native.get_proc_addr(native.inst, "vkGetPhysicalDeviceProperties2"));
    if (get_properties == nullptr) {
        return false;
    }

    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    get_properties(native.phys_dev, &properties);

    std::string actual_uuid;
    for (const auto byte : identity.deviceUUID) {
        actual_uuid += std::format("{:02x}", byte);
    }
    return actual_uuid == expected_uuid;
}

int create_matching_device(device_owner_s& owner, const gpu::device_diagnostics_s& diagnostics)
{
    if (!diagnostics.selected_uuid) {
        return av_hwdevice_ctx_create(&owner.source, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0);
    }

    // FFmpeg 9.0.2 uses vkEnumeratePhysicalDevices order without sorting, as
    // does our diagnostic list (including rejected devices). Its public device
    // selector accepts an index or name, not a UUID.
    const auto selected =
        std::ranges::find(diagnostics.devices, *diagnostics.selected_uuid, &gpu::physical_device_diagnostics_s::uuid);
    if (selected == diagnostics.devices.end()) {
        return AVERROR(ENODEV);
    }

    const auto preferred = static_cast<size_t>(std::distance(diagnostics.devices.begin(), selected));
    for (size_t attempt = 0; attempt < diagnostics.devices.size(); ++attempt) {
        // Try the expected index first, then each remaining index once. Separate
        // instances/layers may enumerate in another order; UUID is authoritative.
        auto index = preferred;
        if (attempt != 0) {
            index = attempt - 1;
            if (index >= preferred) {
                ++index;
            }
        }
        const auto selection = std::to_string(index);
        const int  result =
            av_hwdevice_ctx_create(&owner.source, AV_HWDEVICE_TYPE_VULKAN, selection.c_str(), nullptr, 0);
        if (result < 0) {
            continue;
        }

        const auto* context = reinterpret_cast<AVHWDeviceContext*>(owner.source->data);
        const auto* native  = static_cast<AVVulkanDeviceContext*>(context->hwctx);
        if (matches_device(*native, *diagnostics.selected_uuid)) {
            return 0;
        }

        av_buffer_unref(&owner.source);
    }

    getlog("ffmpeg")->info("No FFmpeg Vulkan device matched compositor GPU {}; trying the next hardware backend",
                           *diagnostics.selected_uuid);
    return AVERROR(ENODEV);
}

} // namespace

int create_vulkan_decode_device(AVBufferRef** device, const gpu::device_diagnostics_s& diagnostics)
{
    // Let upstream FFmpeg create its Vulkan device. Expose a public borrowed-device
    // context with only playback capabilities. Encoding support otherwise adds
    // ENCODE_SRC/profile-independent flags to decode images, which need not be
    // valid for the decoder's queried video profile on the driver.
    auto owner  = std::make_unique<device_owner_s>();
    int  result = create_matching_device(*owner, diagnostics);
    if (result < 0) {
        return result;
    }

    auto* source = reinterpret_cast<AVHWDeviceContext*>(owner->source->data);
    auto* native = static_cast<AVVulkanDeviceContext*>(source->hwctx);
    for (int i = 0; i < native->nb_enabled_dev_extensions; ++i) {
        const std::string_view name{native->enabled_dev_extensions[i]};
        if (!name.starts_with("VK_KHR_video_encode") && !name.starts_with("VK_EXT_video_encode")) {
            owner->extensions.push_back(native->enabled_dev_extensions[i]);
        }
    }

    AVBufferRef* borrowed = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (borrowed == nullptr) {
        return AVERROR(ENOMEM);
    }

    auto* context = reinterpret_cast<AVHWDeviceContext*>(borrowed->data);
    auto* target  = static_cast<AVVulkanDeviceContext*>(context->hwctx);
    // Copy the public device description, leaving FFmpeg's per-context queue
    // locks unset so initialization creates locks owned by the borrowed context.
    target->alloc                      = native->alloc;
    target->get_proc_addr              = native->get_proc_addr;
    target->inst                       = native->inst;
    target->phys_dev                   = native->phys_dev;
    target->act_dev                    = native->act_dev;
    target->device_features            = native->device_features;
    target->enabled_inst_extensions    = native->enabled_inst_extensions;
    target->nb_enabled_inst_extensions = native->nb_enabled_inst_extensions;
    target->queue_flags                = native->queue_flags;
    target->enabled_dev_extensions     = owner->extensions.data();
    target->nb_enabled_dev_extensions  = static_cast<int>(owner->extensions.size());
    target->nb_qf                      = 0;
    for (int i = 0; i < native->nb_qf; ++i) {
        auto family  = std::span(native->qf)[i];
        family.flags = static_cast<VkQueueFlagBits>(family.flags & ~VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
        if (family.flags != 0) {
            std::span(target->qf)[target->nb_qf++] = family;
        }
    }

    context->user_opaque = owner.release();
    context->free = [](AVHWDeviceContext* released) { delete static_cast<device_owner_s*>(released->user_opaque); };
    result        = av_hwdevice_ctx_init(borrowed);
    if (result < 0) {
        av_buffer_unref(&borrowed);
        return result;
    }

    *device = borrowed;
    return 0;
}

int configure_vulkan_decode_frames(AVCodecContext* codec)
{
    AVBufferRef* reference = nullptr;
    int          result = avcodec_get_hw_frames_parameters(codec, codec->hw_device_ctx, AV_PIX_FMT_VULKAN, &reference);
    if (result < 0) {
        return result;
    }

    auto* frames    = reinterpret_cast<AVHWFramesContext*>(reference->data);
    auto* vk_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
    // FFmpeg adds storage/sample/transfer usages during pool initialization.
    // Check that the video profile supports that combined usage before FFmpeg
    // allocates its first image; otherwise try the next native decoder backend.
    auto* native =
        static_cast<AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(codec->hw_device_ctx->data)->hwctx);
    VolkInstanceTable dispatch{};
    volkLoadInstanceTable(&dispatch, native->inst);
    VkPhysicalDeviceVideoFormatInfoKHR info{
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
        .pNext      = vk_frames->create_pnext,
        .imageUsage = static_cast<VkImageUsageFlags>(vk_frames->usage) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    uint32_t count = 0;
    auto     query = dispatch.vkGetPhysicalDeviceVideoFormatPropertiesKHR;
    auto queried   = query != nullptr ? query(native->phys_dev, &info, &count, nullptr) : VK_ERROR_FEATURE_NOT_PRESENT;
    std::vector<VkVideoFormatPropertiesKHR> properties(count, {.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    if (queried == VK_SUCCESS && count != 0) {
        queried = query(native->phys_dev, &info, &count, properties.data());
    }

    const VkImageCreateFlags flags     = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    bool                     supported = false;
    if (queried == VK_SUCCESS) {
        for (const auto& property : properties) {
            if (property.format == vk_frames->format[0] && property.imageType == VK_IMAGE_TYPE_2D &&
                property.imageTiling == vk_frames->tiling && (property.imageCreateFlags & flags) == flags) {
                supported = true;
            }
        }
    }

    if (!supported) {
        getlog("ffmpeg")->info(
            "Vulkan decode profile cannot support FFmpeg's component image usages; trying the next hardware backend");
        av_buffer_unref(&reference);
        return AVERROR(ENOSYS);
    }

    vk_frames->img_flags = flags;
    result               = av_hwframe_ctx_init(reference);
    if (result < 0) {
        av_buffer_unref(&reference);
        return result;
    }

    av_buffer_unref(&codec->hw_frames_ctx);
    codec->hw_frames_ctx = reference;
    return 0;
}

} // namespace miximus::nodes::ffmpeg::detail
