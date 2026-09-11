#include "detail/device.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace miximus::gpu::detail {

recording_state_s::recording_state_s(std::shared_ptr<recording_context_state_s> recording_context, arena_s* slot)
    : owner(recording_context->owner)
    , context(std::move(recording_context))
    , arena(slot)
{
    check(owner->vk.vkResetCommandPool(owner->device, arena->pool, 0), "reset command arena");
    for (auto pool : arena->descriptor_pools) {
        check(owner->vk.vkResetDescriptorPool(owner->device, pool, 0), "reset descriptor arena");
    }
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(owner->vk.vkBeginCommandBuffer(arena->commands, &info), "begin recording");
}

recording_state_s::~recording_state_s()
{
    // Tentative image layouts are committed only by successful submission.
    for (const auto& resource : resources) {
        resource->recording_uses.fetch_sub(1);
    }
    arena->reserved.store(false, std::memory_order_release);
}

void recording_state_s::retain(const std::shared_ptr<resource_state_s>& resource)
{
    if (submission_attempted) {
        throw std::logic_error("recording already submitted");
    }

    if (!resource || resource->owner != owner) {
        throw std::invalid_argument("resource belongs to another Vulkan device");
    }

    if (std::ranges::find(resources, resource) == resources.end()) {
        resources.push_back(resource);
        resource->recording_uses.fetch_add(1);
    }
}

VkDescriptorSet recording_state_s::allocate_descriptor(VkDescriptorSetLayout layout)
{
    // Grow in reusable pages as the graph needs more draws. A busy GPU limits
    // the number of recordings in flight, not the number of draws in a frame.
    const auto page_index = descriptor_requests / owner->options.descriptor_page_size;
    if (page_index == arena->descriptor_pools.size()) {
        const auto                                sets = owner->options.descriptor_page_size;
        const std::array<VkDescriptorPoolSize, 3> sizes{
            {
             {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = sets * 2},
             {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = sets},
             {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = sets},
             }
        };

        VkDescriptorPoolCreateInfo create{};
        create.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        create.maxSets       = sets;
        create.poolSizeCount = static_cast<uint32_t>(sizes.size());
        create.pPoolSizes    = sizes.data();

        // Establish ownership before allocating so a failed allocation or an
        // abandoned recording cannot leak a native descriptor pool.
        auto&      pool   = arena->descriptor_pools.emplace_back();
        const auto result = owner->vk.vkCreateDescriptorPool(owner->device, &create, nullptr, &pool);
        if (result != VK_SUCCESS) {
            arena->descriptor_pools.pop_back();
            if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                throw recording_unavailable_s{"GPU descriptor memory exhausted"};
            }
            check(result, "create descriptor page");
        }
    }

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool     = arena->descriptor_pools.at(page_index);
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts        = &layout;

    VkDescriptorSet descriptor{};
    const auto      result = owner->vk.vkAllocateDescriptorSets(owner->device, &allocate, &descriptor);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        throw recording_unavailable_s{"GPU descriptor memory exhausted"};
    }
    check(result, "allocate descriptor");
    ++descriptor_requests;
    return descriptor;
}

void recording_state_s::transition(const std::shared_ptr<texture_state_s>& image,
                                   VkImageLayout                           layout,
                                   VkPipelineStageFlags2                   stage,
                                   VkAccessFlags2                          access,
                                   uint32_t                                mip)
{
    retain(image);

    // Layout transitions remain local until submit succeeds; an aborted recording
    // must not change the starting layout seen by the next recorder.
    auto [it, inserted] = layouts.try_emplace(image.get(), image->mip_levels, VK_IMAGE_LAYOUT_UNDEFINED);
    static_cast<void>(inserted);
    auto& previous = it->second.at(mip);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask =
        previous == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask       = previous == VK_IMAGE_LAYOUT_UNDEFINED
                                      ? VK_ACCESS_2_NONE
                                      : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask        = stage;
    barrier.dstAccessMask       = access;
    barrier.oldLayout           = previous;
    barrier.newLayout           = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image->image;
    barrier.subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .baseMipLevel   = mip,
                                   .levelCount     = 1,
                                   .baseArrayLayer = 0,
                                   .layerCount     = 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    if (previous == VK_IMAGE_LAYOUT_UNDEFINED) {
        auto [uses, unused] = initial_uses.try_emplace(image.get(), image->mip_levels);
        static_cast<void>(unused);
        uses->second.at(mip) = {.layout = layout, .stage = stage, .access = access};
    } else {
        owner->vk.vkCmdPipelineBarrier2(arena->commands, &dependency);
    }
    previous = layout;

    if (mip == 0 && ((access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0U)) {
        image->content_version.fetch_add(1, std::memory_order_relaxed);
        mip_dirty[image.get()] = true;
    }
}

void recording_state_s::prepare_sampled(const std::shared_ptr<texture_state_s>& image, bool minifying)
{
    retain(image);
    const auto levels = minifying ? image->mip_levels : 1U;
    auto [dirty, inserted] =
        mip_dirty.try_emplace(image.get(), image->content_version.load() != image->mip_version.load());
    static_cast<void>(inserted);
    // Explicit producer generation and defensive consumer preparation share this
    // path. Abandoned commands cannot mark a dirty mip chain clean.
    if (levels > 1 && dirty->second) {
        auto width  = static_cast<int32_t>(image->extent.width);
        auto height = static_cast<int32_t>(image->extent.height);
        for (uint32_t mip = 1; mip < levels; ++mip) {
            transition(image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT,
                       mip - 1);
            transition(image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       mip);
            const int32_t next_width  = std::max(1, width / 2);
            const int32_t next_height = std::max(1, height / 2);

            VkImageBlit blit{};
            blit.srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip - 1, .baseArrayLayer = 0, .layerCount = 1};
            blit.srcOffsets[1]  = {.x = width, .y = height, .z = 1};
            blit.dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip, .baseArrayLayer = 0, .layerCount = 1};
            blit.dstOffsets[1] = {.x = next_width, .y = next_height, .z = 1};
            owner->vk.vkCmdBlitImage(arena->commands,
                                     image->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     image->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     1,
                                     &blit,
                                     VK_FILTER_LINEAR);
            width  = next_width;
            height = next_height;
        }

        dirty->second               = false;
        generated_mips[image.get()] = image->content_version.load();
    }

    for (uint32_t mip = 0; mip < levels; ++mip) {
        transition(image,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   mip);
    }
}

void recording_state_s::buffer_barrier(const std::shared_ptr<buffer_state_s>& buffer,
                                       VkPipelineStageFlags2                  stage,
                                       VkAccessFlags2                         access)
{
    retain(buffer);
    buffer->flush_host_writes();

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    barrier.dstStageMask  = stage;
    barrier.dstAccessMask = access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer              = buffer->buffer;
    barrier.size                = VK_WHOLE_SIZE;

    VkDependencyInfo dependency{};
    dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers    = &barrier;
    owner->vk.vkCmdPipelineBarrier2(arena->commands, &dependency);
}

completion_s recording_state_s::submit(std::span<const VkSemaphoreSubmitInfo> wait_semaphores,
                                       std::span<const VkSemaphoreSubmitInfo> signal_semaphores)
{
    if (submission_attempted) {
        throw std::logic_error("recording already submitted");
    }
    submission_attempted = true;
    check(owner->vk.vkEndCommandBuffer(arena->commands), "end recording");
    waits.assign(wait_semaphores.begin(), wait_semaphores.end());
    signals.assign(signal_semaphores.begin(), signal_semaphores.end());
    submission = std::make_shared<submission_state_s>();
    sequence   = context->next_sequence.fetch_add(1);
    return {owner, submission};
}
} // namespace miximus::gpu::detail

namespace miximus::gpu {

recording_s::recording_s(std::unique_ptr<detail::recording_state_s> state)
    : state_(std::move(state))
{
}

recording_s::~recording_s() = default;

std::unique_ptr<recording_s> device_s::try_record() { return default_context_->try_record(); }

namespace {

VkBufferImageCopy copy_region(const detail::texture_state_s& image, size_t buffer_bytes, size_t stride)
{
    const auto   bpp       = detail::texel_bytes(image.format);
    const size_t row_bytes = size_t{image.extent.width} * bpp;
    if (stride == 0) {
        stride = row_bytes;
    }

    if (stride < row_bytes || stride % bpp != 0 || stride / bpp > UINT32_MAX ||
        image.extent.height > buffer_bytes / stride) {
        throw std::invalid_argument("invalid transfer stride or buffer extent");
    }

    VkBufferImageCopy region{};
    region.bufferRowLength  = static_cast<uint32_t>(stride / bpp);
    region.imageSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    region.imageExtent = {.width = image.extent.width, .height = image.extent.height, .depth = 1};
    return region;
}
} // namespace

void recording_s::upload(const buffer_s& source, const texture_s& destination, size_t row_stride)
{
    if (!state_ || !source || !destination) {
        throw std::invalid_argument("invalid upload handles");
    }

    const auto region = copy_region(*destination.state_, source.size(), row_stride);
    state_->retain(source.state_);
    state_->buffer_barrier(source.state_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    state_->transition(destination.state_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
    state_->owner->vk.vkCmdCopyBufferToImage(state_->arena->commands,
                                             source.state_->buffer,
                                             destination.state_->image,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             1,
                                             &region);
}

void recording_s::readback(const texture_s& source, const buffer_s& destination, size_t row_stride)
{
    if (!state_ || !source || !destination) {
        throw std::invalid_argument("invalid readback handles");
    }

    const auto region = copy_region(*source.state_, destination.size(), row_stride);
    state_->buffer_barrier(destination.state_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    state_->transition(source.state_,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    state_->owner->vk.vkCmdCopyImageToBuffer(state_->arena->commands,
                                             source.state_->image,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             destination.state_->buffer,
                                             1,
                                             &region);
    if (destination.state_->mapped != nullptr) {
        state_->buffer_barrier(destination.state_, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
}

void recording_s::copy(const buffer_s& source, const buffer_s& destination, size_t bytes)
{
    if (!state_ || !source || !destination) {
        throw std::invalid_argument("invalid copy handles");
    }

    if (source.state_ == destination.state_ || (bytes == 0U) || bytes > source.size() || bytes > destination.size()) {
        throw std::invalid_argument("invalid buffer copy");
    }

    state_->retain(source.state_);
    state_->buffer_barrier(source.state_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    state_->buffer_barrier(destination.state_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkBufferCopy region{0, 0, bytes};
    state_->owner->vk.vkCmdCopyBuffer(
        state_->arena->commands, source.state_->buffer, destination.state_->buffer, 1, &region);
    if (destination.state_->mapped != nullptr) {
        state_->buffer_barrier(destination.state_, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
}

void recording_s::generate_mip_maps(const texture_s& texture) { state_->prepare_sampled(texture.state_, true); }

void recording_s::clear(const texture_s& target, std::array<float, 4> color)
{
    if (!state_ || !target) {
        throw std::invalid_argument("invalid clear handles");
    }

    state_->transition(target.state_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkClearColorValue value{};
    if (target.format() == format_e::r32_uint) {
        if (std::ranges::any_of(color, [](float c) { return c != 0; })) {
            throw std::invalid_argument("integer clear accepts only zero");
        }
    } else {
        std::ranges::copy(color, value.float32);
    }

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    state_->owner->vk.vkCmdClearColorImage(
        state_->arena->commands, target.state_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

void recording_s::draw(const texture_s& source, const texture_s& destination, const draw_s& parameters)
{
    if (!state_ || !source || !destination) {
        throw std::invalid_argument("invalid draw handles");
    }

    const auto finite = [](float value) { return std::isfinite(value); };
    if (!std::isfinite(parameters.opacity) || !std::ranges::all_of(parameters.destination, finite) ||
        !std::ranges::all_of(parameters.uv, finite)) {
        throw std::invalid_argument("non-finite draw parameters");
    }

    // The vertex shader shares this prefix with the mix pipeline. Scalar fields
    // preserve the original vec4 lane offsets while making each value explicit.
    struct draw_parameters_s
    {
        std::array<float, 4> destination_rectangle{};
        std::array<float, 4> source_rectangle{};
        float                opacity{};
        float                target_width{};
        float                target_height{};
        float                mix_fraction{};
        color_operation_e    color_operation{};
        channel_order_e      input_order{};
        channel_order_e      output_order{};

        // Match the shader block's explicit tail padding to 64 bytes.
        int32_t memory_layout_padding{};
    };

    static_assert(sizeof(draw_parameters_s) == 64);
    static_assert(offsetof(draw_parameters_s, source_rectangle) == 16);
    static_assert(offsetof(draw_parameters_s, opacity) == 32);
    static_assert(offsetof(draw_parameters_s, color_operation) == 48);
    static_assert(offsetof(draw_parameters_s, input_order) == 52);
    static_assert(offsetof(draw_parameters_s, output_order) == 56);
    static_assert(offsetof(draw_parameters_s, memory_layout_padding) == 60);

    draw_parameters_s push{
        .destination_rectangle = parameters.destination,
        .source_rectangle      = parameters.uv,
        .opacity               = parameters.opacity,
        .target_width          = static_cast<float>(destination.extent().width),
        .target_height         = static_cast<float>(destination.extent().height),
        .color_operation       = parameters.transfer,
        .input_order           = parameters.input_order,
        .output_order          = parameters.output_order,
    };

    if (push.destination_rectangle[2] == 0 && push.destination_rectangle[3] == 0) {
        push.destination_rectangle = {0, 0, push.target_width, push.target_height};
    }

    const bool minifying = std::abs(push.source_rectangle[2]) * static_cast<float>(source.extent().width) >
                               std::abs(push.destination_rectangle[2]) ||
                           std::abs(push.source_rectangle[3]) * static_cast<float>(source.extent().height) >
                               std::abs(push.destination_rectangle[3]);
    state_->draw(source.state_,
                 source.state_,
                 destination.state_,
                 std::as_bytes(std::span(&push, 1)),
                 parameters.compositing,
                 detail::draw_operation_e::texture,
                 {minifying, minifying},
                 parameters.clip);
}

void recording_s::mix(const texture_s& a, const texture_s& b, const texture_s& destination, const mix_s& parameters)
{
    if (!state_ || !a || !b || !destination) {
        throw std::invalid_argument("invalid mix handles");
    }

    const auto finite = [](float value) { return std::isfinite(value); };
    if (!std::isfinite(parameters.fraction) || !std::ranges::all_of(parameters.a_destination, finite) ||
        !std::ranges::all_of(parameters.b_destination, finite) || !std::ranges::all_of(parameters.a_uv, finite) ||
        !std::ranges::all_of(parameters.b_uv, finite)) {
        throw std::invalid_argument("non-finite mix parameters");
    }

    struct mix_parameters_s
    {
        std::array<float, 4> destination_rectangle{};
        std::array<float, 4> source_rectangle{};
        float                opacity{};
        float                target_width{};
        float                target_height{};
        float                mix_fraction{};
        std::array<float, 4> a_destination{};
        std::array<float, 4> a_source{};
        std::array<float, 4> b_destination{};
        std::array<float, 4> b_source{};
        int32_t              video_mix{};

        // Match the shader block's explicit tail padding to 128 bytes.
        std::array<int32_t, 3> memory_layout_padding{};
    };

    static_assert(sizeof(mix_parameters_s) == 128);
    static_assert(offsetof(mix_parameters_s, source_rectangle) == 16);
    static_assert(offsetof(mix_parameters_s, opacity) == 32);
    static_assert(offsetof(mix_parameters_s, mix_fraction) == 44);
    static_assert(offsetof(mix_parameters_s, a_destination) == 48);
    static_assert(offsetof(mix_parameters_s, a_source) == 64);
    static_assert(offsetof(mix_parameters_s, b_destination) == 80);
    static_assert(offsetof(mix_parameters_s, b_source) == 96);
    static_assert(offsetof(mix_parameters_s, video_mix) == 112);
    static_assert(offsetof(mix_parameters_s, memory_layout_padding) == 116);

    const auto             target_width  = static_cast<float>(destination.extent().width);
    const auto             target_height = static_cast<float>(destination.extent().height);
    const mix_parameters_s push{
        .destination_rectangle = {0, 0, target_width, target_height},
        .source_rectangle      = {0, 0, 1,            1            },
        .opacity               = 1,
        .target_width          = target_width,
        .target_height         = target_height,
        .mix_fraction          = parameters.fraction,
        .a_destination         = parameters.a_destination,
        .a_source              = parameters.a_uv,
        .b_destination         = parameters.b_destination,
        .b_source              = parameters.b_uv,
        .video_mix             = parameters.blend_mode == blend_mode_e::video ? 1 : 0,
    };

    const auto minifies = [&](const texture_s& image, const auto& rect, const auto& uv) {
        return std::abs(uv[2]) * image.extent().width > std::abs(rect[2]) * destination.extent().width ||
               std::abs(uv[3]) * image.extent().height > std::abs(rect[3]) * destination.extent().height;
    };

    state_->draw(a.state_,
                 b.state_,
                 destination.state_,
                 std::as_bytes(std::span(&push, 1)),
                 parameters.compositing,
                 detail::draw_operation_e::mix,
                 {minifies(a, parameters.a_destination, parameters.a_uv),
                  minifies(b, parameters.b_destination, parameters.b_uv)},
                 parameters.clip);
}

void recording_s::on_submitted(std::function<void(completion_s)> publish)
{
    if (!state_) {
        throw std::logic_error("recording already submitted");
    }

    state_->publications.emplace_back(std::move(publish));
}

void recording_s::wait_for(const completion_s& dependency)
{
    if (!state_ || state_->submission_attempted) {
        throw std::logic_error("dependency added after recording submission");
    }
    if (!dependency) {
        return;
    }
    if (dependency.state_ != state_->owner) {
        throw std::invalid_argument("GPU dependency belongs to another device");
    }
    state_->dependencies.push_back(dependency);
}

completion_s recording_s::submit()
{
    if (!state_) {
        throw std::logic_error("recording already submitted");
    }
    return detail::enqueue_recording(state_);
}
} // namespace miximus::gpu
