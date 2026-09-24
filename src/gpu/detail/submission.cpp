#include "device.hpp"
#include "fatal.hpp"
#include "logger/logger.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace miximus::gpu::detail {

completion_s enqueue_recording(std::unique_ptr<recording_state_s>&    recording,
                               std::span<const VkSemaphoreSubmitInfo> waits,
                               std::span<const VkSemaphoreSubmitInfo> signals)
{
    const auto completion = recording->submit(waits, signals);
    auto       owner      = recording->owner;
    auto*      arena      = recording->arena;
    // The reserved arena owns this handoff until the worker adopts it into RAII.
    arena->queued.store(recording.release(), std::memory_order_release);
    owner->submissions.wake.notify_one();
    return completion;
}

void recording_context_state_s::initialize(uint32_t capacity)
{
    if (capacity == 0 || capacity > 64) {
        throw std::invalid_argument("invalid recording context capacity");
    }

    for (uint32_t index = 0; index < capacity; ++index) {
        auto&                   arena = *arenas.emplace_back(std::make_unique<arena_s>());
        VkCommandPoolCreateInfo pool{};
        pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = owner->queue_family;
        check(owner->vk.vkCreateCommandPool(owner->device, &pool, nullptr, &arena.pool), "create command pool");

        VkCommandBufferAllocateInfo allocation{};
        allocation.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool        = arena.pool;
        allocation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(owner->vk.vkAllocateCommandBuffers(owner->device, &allocation, &arena.commands), "allocate commands");
        check(owner->vk.vkAllocateCommandBuffers(owner->device, &allocation, &arena.prologue), "allocate prologue");
    }
}

recording_context_state_s::~recording_context_state_s()
{
    // Queued and in-flight recordings retain this context. Its last reference can
    // disappear only after every pool is safe to destroy.
    for (const auto& arena : arenas) {
        for (auto pool : arena->descriptor_pools) {
            owner->vk.vkDestroyDescriptorPool(owner->device, pool, nullptr);
        }
        if (arena->pool != VK_NULL_HANDLE) {
            owner->vk.vkDestroyCommandPool(owner->device, arena->pool, nullptr);
        }
    }
}

std::unique_ptr<recording_state_s> recording_context_state_s::try_record()
{
    if (owner->submissions.submission_failed.load() || owner->submissions.stopping.load()) {
        throw std::runtime_error("GPU submission service is unavailable");
    }

    for (const auto& arena : arenas) {
        bool expected = false;
        if (arena->reserved.compare_exchange_strong(expected, true)) {
            try {
                return std::make_unique<recording_state_s>(shared_from_this(), arena.get());
            } catch (...) {
                arena->reserved.store(false);
                throw;
            }
        }
    }
    return nullptr;
}

void recording_state_s::record_prologue()
{
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(owner->vk.vkBeginCommandBuffer(arena->prologue, &begin), "begin resource prologue");

    for (const auto& [image, uses] : initial_uses) {
        for (uint32_t mip = 0; mip < uses.size(); ++mip) {
            const auto& use = uses[mip];
            if (use.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                continue;
            }

            VkImageMemoryBarrier2 barrier{};
            barrier.sType     = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.oldLayout = image->layouts.at(mip);
            barrier.newLayout = use.layout;
            if (barrier.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            }
            barrier.dstStageMask        = use.stage;
            barrier.dstAccessMask       = use.access;
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

            // The body always loads attachment contents. Only submission order
            // tells us whether this is a new target or an earlier draw must survive.
            if (barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                use.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                owner->vk.vkCmdPipelineBarrier2(arena->prologue, &dependency);

                const VkClearColorValue clear{};
                owner->vk.vkCmdClearColorImage(arena->prologue,
                                               image->image,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               &clear,
                                               1,
                                               &barrier.subresourceRange);

                barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout     = use.layout;
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask  = use.stage;
                barrier.dstAccessMask = use.access;
            }
            owner->vk.vkCmdPipelineBarrier2(arena->prologue, &dependency);
        }
    }
    check(owner->vk.vkEndCommandBuffer(arena->prologue), "end resource prologue");
}

void recording_state_s::submit_native()
{
    uint64_t dependency_value{};
    for (const auto& dependency : dependencies) {
        if (!dependency.submitted()) {
            throw std::logic_error("unsubmitted GPU dependency reached native submission");
        }
        dependency_value = std::max(dependency_value, dependency.submission_->value.load(std::memory_order_acquire));
    }
    if (dependency_value != 0) {
        VkSemaphoreSubmitInfo wait{};
        wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = owner->submissions.timeline;
        wait.value     = dependency_value;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waits.push_back(wait);
    }
    record_prologue();

    std::array<VkCommandBufferSubmitInfo, 2> commands{};
    commands[0].sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commands[0].commandBuffer = arena->prologue;
    commands[1].sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commands[1].commandBuffer = arena->commands;

    const auto            value = owner->submissions.last_submitted_timeline_value + 1;
    VkSemaphoreSubmitInfo timeline{};
    timeline.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    timeline.semaphore = owner->submissions.timeline;
    timeline.value     = value;
    timeline.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals.push_back(timeline);

    VkSubmitInfo2 batch{};
    batch.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    batch.waitSemaphoreInfoCount   = static_cast<uint32_t>(waits.size());
    batch.pWaitSemaphoreInfos      = waits.data();
    batch.commandBufferInfoCount   = static_cast<uint32_t>(commands.size());
    batch.pCommandBufferInfos      = commands.data();
    batch.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
    batch.pSignalSemaphoreInfos    = signals.data();
    {
        const std::scoped_lock lock(owner->submissions.queue_mutex);
        check(owner->vk.vkQueueSubmit2(owner->submissions.queue, 1, &batch, VK_NULL_HANDLE), "queue submission");
    }

    owner->submissions.last_submitted_timeline_value = value;
    for (const auto& [image, final_layouts] : layouts) {
        for (size_t mip = 0; mip < final_layouts.size(); ++mip) {
            if (final_layouts[mip] != VK_IMAGE_LAYOUT_UNDEFINED) {
                image->layouts[mip] = final_layouts[mip];
            }
        }
    }
    for (const auto& [image, version] : generated_mips) {
        if (!mip_dirty.at(image)) {
            image->mip_version.store(version);
        }
    }
    for (const auto& resource : resources) {
        resource->last_use_timeline_value.store(value);
    }

    submission->value.store(value, std::memory_order_release);
    const completion_s completed(owner, submission);
    for (auto& publish : publications) {
        publish(completed);
    }
    publications.clear();
}

void submission_engine_s::start()
{
    submission_worker = std::thread([this] { run(); });
}

void submission_engine_s::stop()
{
    stopping.store(true);
    wake.notify_one();
    if (submission_worker.joinable()) {
        submission_worker.join();
    }
}

namespace {

bool dependencies_submitted(const recording_state_s& recording)
{
    try {
        return std::ranges::all_of(recording.dependencies,
                                   [](const auto& dependency) { return dependency.submitted(); });
    } catch (const std::exception&) {
        // Allow submission to consume failed dependencies: submit_native reports
        // their failure through the ticket instead of leaving this recording queued.
        return true;
    }
}

std::unique_ptr<recording_state_s> take_next_recording(recording_context_state_s& context)
{
    // A freed arena can be reused before an older arena. Follow enqueue order,
    // not the physical arena index, when recording the resource prologues.
    for (const auto& arena : context.arenas) {
        auto* queued = arena->queued.load(std::memory_order_acquire);
        if (queued != nullptr && queued->sequence == context.submitted_sequence) {
            // Never put a wait for a not-yet-submitted producer on the shared
            // graphics queue: that producer might need this same worker/queue.
            // Failed dependencies are consumed below to propagate their failure.
            if (!dependencies_submitted(*queued)) {
                return nullptr;
            }
            ++context.submitted_sequence;
            return std::unique_ptr<recording_state_s>(arena->queued.exchange(nullptr));
        }
    }
    return nullptr;
}

void submit_recording(device_state_s& device, recording_state_s& recording)
{
    try {
        if (device.submissions.submission_failed.load()) {
            recording.submission->failed.store(true);
            return;
        }
        recording.submit_native();
    } catch (const std::exception& error) {
        recording.submission->failed.store(true);
        device.submissions.submission_failed.store(true);
        fatal_gpu_error(std::format("GPU submission failed: {}", error.what()));
    }
}

} // namespace

void submission_engine_s::run()
{
    std::vector<std::unique_ptr<recording_state_s>>         in_flight;
    std::vector<std::shared_ptr<recording_context_state_s>> active;

    while (true) {
        active.clear();
        {
            const std::scoped_lock lock(contexts_mutex);
            std::erase_if(contexts, [&active](const auto& weak) {
                if (auto context = weak.lock()) {
                    active.push_back(std::move(context));
                    return false;
                }
                return true;
            });
        }

        bool submitted = false;
        for (const auto& context : active) {
            auto record = take_next_recording(*context);
            if (!record) {
                continue;
            }

            // Reserve retirement ownership before calling the driver. Even a
            // publication callback failure cannot destroy an accepted GPU job.
            in_flight.push_back(std::move(record));
            submit_recording(owner, *in_flight.back());
            submitted = true;

            // One submission per context per pass prevents a busy producer
            // from monopolizing the queue while the renderer is ready.
        }

        uint64_t   completed{};
        const auto result = owner.vk.vkGetSemaphoreCounterValue(owner.device, timeline, &completed);
        if (result != VK_SUCCESS) {
            fatal_gpu_error(std::format("GPU completion query failed: Vulkan result {}", static_cast<int>(result)));
        } else {
            std::erase_if(in_flight,
                          [completed](const auto& record) { return record->submission->value.load() <= completed; });

            // Release CPU recording pins before exposing GPU completion to host readers.
            completed_value.store(completed, std::memory_order_release);
        }
        owner.collect();

        if (stopping.load() && !submitted && in_flight.empty()) {
            break;
        }
        if (!submitted) {
            std::unique_lock lock(wake_mutex);
            wake.wait_for(lock, std::chrono::microseconds(100));
        }
    }
}

} // namespace miximus::gpu::detail

namespace miximus::gpu {
recording_context_s::recording_context_s(std::shared_ptr<detail::recording_context_state_s> state)
    : state_(std::move(state))
{
}

std::unique_ptr<recording_s> recording_context_s::try_record()
{
    auto recording = state_->try_record();
    return recording ? std::unique_ptr<recording_s>(new recording_s(std::move(recording))) : nullptr;
}

recording_context_s device_s::create_recording_context(uint32_t capacity)
{
    auto context   = std::make_shared<detail::recording_context_state_s>();
    context->owner = state_;
    context->initialize(capacity);
    {
        const std::scoped_lock lock(state_->submissions.contexts_mutex);
        state_->submissions.contexts.push_back(context);
    }
    return recording_context_s(std::move(context));
}
} // namespace miximus::gpu

namespace miximus::gpu::detail {
void submission_engine_s::initialize()
{
    // Queue acceptance assigns values; the submission worker publishes completed values.
    VkSemaphoreTypeCreateInfo timeline_info{};
    timeline_info.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore.pNext = &timeline_info;
    check(owner.vk.vkCreateSemaphore(owner.device, &semaphore, nullptr, &timeline), "timeline semaphore");
}

void submission_engine_s::destroy_timeline()
{
    if (timeline != VK_NULL_HANDLE) {
        owner.vk.vkDestroySemaphore(owner.device, timeline, nullptr);
        timeline = VK_NULL_HANDLE;
    }
}
} // namespace miximus::gpu::detail
