#include "cuda_staging.hpp"

#include "gpu/detail/device.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <cstring>
#include <cuda_runtime_api.h>
#include <format>
#include <limits>
#include <memory>
#include <thread>
#include <unistd.h>

namespace miximus::gpu::transfer::detail {
namespace {

void check_cuda(cudaError_t result, const char* operation)
{
    if (result != cudaSuccess) {
        throw std::runtime_error(std::format("{}: {}", operation, cudaGetErrorString(result)));
    }
}

using gpu::detail::check;
} // namespace

struct cuda_staging_s::state_s
{
    device_s&                                    device;
    recording_context_s*                         recording_context{};
    std::shared_ptr<gpu::detail::device_state_s> owner;
    texture_frame_s&                             frame;
    texture_transfer_plan_s                      plan;
    transfer_direction_e                         direction;
    int                                          cuda_device_index{-1};
    buffer_s                                     shared_transfer_buffer;

    cudaExternalMemory_t imported_memory{};
    void*                device_address{};
    void*                host_base{};
    void*                host_address{};
    size_t               host_allocation_bytes{};
    size_t               device_allocation_bytes{};

    cudaStream_t            stream{};
    cudaEvent_t             copy_completed_event{};
    VkSemaphore             to_cuda{};
    VkSemaphore             to_vulkan{};
    cudaExternalSemaphore_t cuda_wait{};
    cudaExternalSemaphore_t cuda_signal{};
    completion_s            last_submission;
    enum class phase_e : uint8_t
    {
        idle,
        copying,
        acquiring,
        submitted
    } phase{phase_e::idle};
    bool initialized{};

    state_s(device_s&                                    gpu,
            std::shared_ptr<gpu::detail::device_state_s> native,
            texture_frame_s&                             target,
            texture_transfer_plan_s                      layout,
            transfer_direction_e                         transfer_direction)
        : device(gpu)
        , owner(std::move(native))
        , frame(target)
        , plan(layout)
        , direction(transfer_direction)
    {
    }

    state_s(const state_s&)            = delete;
    state_s& operator=(const state_s&) = delete;
    state_s(state_s&&)                 = delete;
    state_s& operator=(state_s&&)      = delete;

    ~state_s()
    {
        // Destruction belongs to the transfer worker, after external host leases drain.
        // Never enqueue a Vulkan wait for CUDA until its completion event is ready.
        try {
            if (last_submission && last_submission.wait(std::chrono::hours(1)) != wait_result_e::ready) {
                throw std::runtime_error("CUDA transfer Vulkan retirement timed out");
            }

            if (cuda_device_index >= 0) {
                (void)cudaSetDevice(cuda_device_index);
            }

            if (stream != nullptr) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
                while (cudaStreamQuery(stream) == cudaErrorNotReady) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        throw std::runtime_error("CUDA transfer retirement timed out");
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        } catch (const std::exception& error) {
            logger::log_error_noexcept("gpu", "{}", error.what());
            std::terminate(); // Freeing an outstanding DMA allocation is not safe.
        }

        if (copy_completed_event != nullptr) {
            (void)cudaEventDestroy(copy_completed_event);
        }

        if (stream != nullptr) {
            (void)cudaStreamDestroy(stream);
        }

        if (cuda_wait != nullptr) {
            (void)cudaDestroyExternalSemaphore(cuda_wait);
        }

        if (cuda_signal != nullptr) {
            (void)cudaDestroyExternalSemaphore(cuda_signal);
        }

        if (device_address != nullptr) {
            (void)cudaFree(device_address);
        }

        if (imported_memory != nullptr) {
            (void)cudaDestroyExternalMemory(imported_memory);
        }

        if (host_base != nullptr) {
            (void)cudaFreeHost(host_base);
        }

        if (to_cuda != nullptr) {
            owner->vk.vkDestroySemaphore(owner->device, to_cuda, nullptr);
        }

        if (to_vulkan != nullptr) {
            owner->vk.vkDestroySemaphore(owner->device, to_vulkan, nullptr);
        }

        // The shared buffer's native allocation retires through its final Vulkan use.
    }
};

cuda_staging_s::cuda_staging_s(device_s&                      device,
                               const texture_transfer_plan_s& plan,
                               transfer_direction_e           direction,
                               texture_frame_s&               frame,
                               recording_context_s*           recording_context)
    : state_(std::make_unique<state_s>(device, device.state_, frame, plan, direction))
{
    state_->recording_context = recording_context;
    auto& state               = *state_;
    if (!state.owner->cuda_external_memory) {
        throw std::runtime_error("selected Vulkan device lacks CUDA external-memory/semaphore FD support");
    }

    select_cuda_device();
    validate_external_resources();
    allocate_host_memory();
    import_transfer_buffer();
    create_external_semaphores();
}

void cuda_staging_s::select_cuda_device()
{
    auto& state = *state_;

    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    state.owner->instance_vk.vkGetPhysicalDeviceProperties2(state.owner->physical, &properties);
    int count{};
    check_cuda(cudaGetDeviceCount(&count), "CUDA device enumeration");
    for (int candidate = 0; candidate < count; ++candidate) {
        cudaDeviceProp cuda_properties{};
        check_cuda(cudaGetDeviceProperties(&cuda_properties, candidate), "CUDA device properties");
        if (std::memcmp(cuda_properties.uuid.bytes, id.deviceUUID, VK_UUID_SIZE) == 0) {
            state.cuda_device_index = candidate;
            break;
        }
    }

    if (state.cuda_device_index < 0) {
        throw std::runtime_error("no CUDA device matches the Vulkan device UUID");
    }

    check_cuda(cudaSetDevice(state.cuda_device_index), "select CUDA device");
}

void cuda_staging_s::validate_external_resources()
{
    auto& state = *state_;

    VkPhysicalDeviceExternalBufferInfo external_info{};
    external_info.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    external_info.usage      = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkExternalBufferProperties external_properties{};
    external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    state.owner->instance_vk.vkGetPhysicalDeviceExternalBufferProperties(
        state.owner->physical, &external_info, &external_properties);
    if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0U) {
        throw std::runtime_error("Vulkan transfer buffer cannot be exported to CUDA");
    }

    VkPhysicalDeviceExternalSemaphoreInfo semaphore_info{};
    semaphore_info.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    semaphore_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkExternalSemaphoreProperties semaphore_properties{};
    semaphore_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    state.owner->instance_vk.vkGetPhysicalDeviceExternalSemaphoreProperties(
        state.owner->physical, &semaphore_info, &semaphore_properties);
    if ((semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0U) {
        throw std::runtime_error("Vulkan semaphore cannot be exported to CUDA");
    }
}

void cuda_staging_s::allocate_host_memory()
{
    auto& state = *state_;

    const size_t host_bytes = state.plan.host_layout.buffer_size_bytes;
    const size_t alignment  = state.plan.host_layout.address_alignment_bytes;
    if (host_bytes > std::numeric_limits<size_t>::max() - alignment || host_bytes > SIZE_MAX - 3) {
        throw std::invalid_argument("CUDA host allocation size overflow");
    }

    // Keep the aligned address stable for every SDK lease of this allocation.
    state.host_allocation_bytes = host_bytes + alignment - 1;
    unsigned host_flags         = cudaHostAllocPortable;
    if (state.direction == transfer_direction_e::cpu_to_gpu &&
        state.plan.host_layout.memory_access == host_memory_access_e::overwrite) {
        host_flags |= cudaHostAllocWriteCombined;
    }

    check_cuda(cudaHostAlloc(&state.host_base, state.host_allocation_bytes, host_flags), "CUDA pinned host allocation");
    state.host_address   = state.host_base;
    auto available_bytes = state.host_allocation_bytes;
    if (std::align(alignment, host_bytes, state.host_address, available_bytes) == nullptr) {
        throw std::logic_error("CUDA host allocation cannot satisfy frame alignment");
    }
    std::memset(state.host_address, 0, host_bytes);
    check_cuda(cudaStreamCreateWithFlags(&state.stream, cudaStreamNonBlocking), "CUDA transfer stream");
    check_cuda(cudaEventCreateWithFlags(&state.copy_completed_event, cudaEventDisableTiming), "CUDA transfer event");
}

void cuda_staging_s::import_transfer_buffer()
{
    auto& state = *state_;

    const size_t host_bytes      = state.plan.host_layout.buffer_size_bytes;
    auto         resource        = std::make_shared<gpu::detail::buffer_state_s>();
    resource->owner              = state.owner;
    resource->bytes              = (host_bytes + 3) & ~size_t{3};
    resource->access             = host_access_e::device_only;
    resource->external_buffer    = true;
    state.shared_transfer_buffer = buffer_s(resource);

    VkExternalMemoryBufferCreateInfo external{};
    external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext       = &external;
    buffer_info.size        = resource->bytes;
    buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(state.owner->vk.vkCreateBuffer(state.owner->device, &buffer_info, nullptr, &resource->buffer),
          "CUDA shared buffer");

    VkMemoryRequirements requirements{};
    state.owner->vk.vkGetBufferMemoryRequirements(state.owner->device, resource->buffer, &requirements);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < state.owner->memory.memoryTypeCount; ++i) {
        if (((requirements.memoryTypeBits & (1U << i)) != 0U) &&
            ((std::span(state.owner->memory.memoryTypes)[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) !=
             0U)) {
            memory_type = i;
            break;
        }
    }

    if (memory_type == UINT32_MAX) {
        throw std::runtime_error("CUDA shared buffer has no device-local memory type");
    }

    // CUDA imports the whole dedicated allocation, not a suballocation from VMA.
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = resource->buffer;

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext       = &dedicated;
    export_info.handleTypes = external.handleTypes;

    VkMemoryAllocateInfo allocate{};
    allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext           = &export_info;
    allocate.allocationSize  = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    check(state.owner->vk.vkAllocateMemory(state.owner->device, &allocate, nullptr, &resource->external_memory),
          "CUDA exportable device allocation");
    check(state.owner->vk.vkBindBufferMemory(state.owner->device, resource->buffer, resource->external_memory, 0),
          "bind CUDA shared buffer");
    state.device_allocation_bytes = requirements.size;
    resource->info                = {.bytes = static_cast<size_t>(requirements.size), .device_local = true};

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory     = resource->external_memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd{-1};
    check(state.owner->vk.vkGetMemoryFdKHR(state.owner->device, &fd_info, &fd), "export Vulkan allocation FD");

    cudaExternalMemoryHandleDesc import{};
    import.type              = cudaExternalMemoryHandleTypeOpaqueFd;
    import.handle.fd         = fd;
    import.size              = requirements.size;
    import.flags             = cudaExternalMemoryDedicated;
    const auto import_result = cudaImportExternalMemory(&state.imported_memory, &import);
    if (import_result != cudaSuccess) {
        (void)::close(fd); // Successful CUDA import owns the FD.
    }

    check_cuda(import_result, "import Vulkan allocation into CUDA");

    cudaExternalMemoryBufferDesc mapped{};
    mapped.size = resource->bytes;
    check_cuda(cudaExternalMemoryGetMappedBuffer(&state.device_address, state.imported_memory, &mapped),
               "map CUDA shared buffer");
}

void cuda_staging_s::create_external_semaphores()
{
    auto& state = *state_;

    const auto make_semaphore = [&](VkSemaphore& semaphore, cudaExternalSemaphore_t& cuda_semaphore) {
        VkExportSemaphoreCreateInfo export_semaphore{};
        export_semaphore.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        export_semaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkSemaphoreCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        create.pNext = &export_semaphore;
        check(state.owner->vk.vkCreateSemaphore(state.owner->device, &create, nullptr, &semaphore),
              "CUDA shared semaphore");

        VkSemaphoreGetFdInfoKHR get{};
        get.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        get.semaphore  = semaphore;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        int semaphore_fd{-1};
        check(state.owner->vk.vkGetSemaphoreFdKHR(state.owner->device, &get, &semaphore_fd),
              "export Vulkan semaphore FD");

        cudaExternalSemaphoreHandleDesc desc{};
        desc.type         = cudaExternalSemaphoreHandleTypeOpaqueFd;
        desc.handle.fd    = semaphore_fd;
        const auto result = cudaImportExternalSemaphore(&cuda_semaphore, &desc);
        if (result != cudaSuccess) {
            (void)::close(semaphore_fd);
        }

        check_cuda(result, "import Vulkan semaphore into CUDA");
    };

    make_semaphore(state.to_cuda, state.cuda_wait);
    make_semaphore(state.to_vulkan, state.cuda_signal);
}

cuda_staging_s::~cuda_staging_s() = default;

void*  cuda_staging_s::host_memory() const noexcept { return state_->host_address; }
size_t cuda_staging_s::host_buffer_size_bytes() const noexcept { return state_->plan.host_layout.buffer_size_bytes; }
size_t cuda_staging_s::allocation_bytes() const noexcept
{
    return state_->host_allocation_bytes + state_->device_allocation_bytes;
}

void cuda_staging_s::prepare_host_write()
{
    if (state_->phase != state_s::phase_e::idle) {
        throw std::logic_error("CUDA host write before transfer completion");
    }
}

void cuda_staging_s::record_ownership_transfer(recording_s& record, ownership_operation_e operation)
{
    auto& state = *state_;
    record.state_->retain(state.shared_transfer_buffer.state_);

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    // Queue-family ownership and memory visibility travel together in each direction.
    if (operation == ownership_operation_e::release_to_cuda) {
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask       = VK_ACCESS_2_NONE;
        barrier.srcQueueFamilyIndex = state.owner->queue_family;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    } else {
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask       = VK_ACCESS_2_NONE;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        barrier.dstQueueFamilyIndex = state.owner->queue_family;
    }

    barrier.buffer = state.shared_transfer_buffer.state_->buffer;
    barrier.size   = VK_WHOLE_SIZE;

    VkDependencyInfo dependency{};
    dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers    = &barrier;
    state.owner->vk.vkCmdPipelineBarrier2(record.state_->arena->commands, &dependency);
}

completion_s cuda_staging_s::submit(recording_s& record, ownership_operation_e operation)
{
    auto& state = *state_;

    VkSemaphoreSubmitInfo semaphore{};
    semaphore.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    semaphore.semaphore = operation == ownership_operation_e::release_to_cuda ? state.to_cuda : state.to_vulkan;
    semaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const auto span     = std::span(&semaphore, 1);
    return operation == ownership_operation_e::release_to_cuda
               ? gpu::detail::enqueue_recording(record.state_, {}, span)
               : gpu::detail::enqueue_recording(record.state_, span, {});
}

bool cuda_staging_s::start_transfer(const completion_s& dependency)
{
    auto& state = *state_;
    auto  record =
        state.recording_context != nullptr ? state.recording_context->try_record() : state.device.try_record();
    if (!record) {
        return false;
    }

    record->wait_for(dependency);

    if (!state.initialized) {
        record->state_->buffer_barrier(
            state.shared_transfer_buffer.state_, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        state.owner->vk.vkCmdFillBuffer(
            record->state_->arena->commands, state.shared_transfer_buffer.state_->buffer, 0, VK_WHOLE_SIZE, 0);
    }

    if (state.direction == transfer_direction_e::gpu_to_cpu) {
        if (state.frame.buffer()) {
            record->copy(state.frame.buffer(), state.shared_transfer_buffer, state.frame.buffer().size());
        } else {
            record->readback(
                *state.frame.texture(), state.shared_transfer_buffer, state.plan.host_layout.row_stride_bytes);
        }
    }

    record_ownership_transfer(*record, ownership_operation_e::release_to_cuda);
    state.last_submission = submit(*record, ownership_operation_e::release_to_cuda);
    state.initialized     = true;

    cudaExternalSemaphoreWaitParams wait{};
    check_cuda(cudaWaitExternalSemaphoresAsync(&state.cuda_wait, &wait, 1, state.stream),
               "CUDA wait for Vulkan release");
    if (state.direction == transfer_direction_e::cpu_to_gpu) {
        check_cuda(cudaMemcpyAsync(state.device_address,
                                   state.host_address,
                                   state.plan.host_layout.buffer_size_bytes,
                                   cudaMemcpyHostToDevice,
                                   state.stream),
                   "CUDA H2D copy");
    } else {
        check_cuda(cudaMemcpyAsync(state.host_address,
                                   state.device_address,
                                   state.plan.host_layout.buffer_size_bytes,
                                   cudaMemcpyDeviceToHost,
                                   state.stream),
                   "CUDA D2H copy");
    }

    cudaExternalSemaphoreSignalParams signal{};
    check_cuda(cudaSignalExternalSemaphoresAsync(&state.cuda_signal, &signal, 1, state.stream),
               "CUDA signal Vulkan acquire");
    check_cuda(cudaEventRecord(state.copy_completed_event, state.stream), "CUDA copy completion event");
    state.phase = state_s::phase_e::copying;
    return true;
}

completion_s cuda_staging_s::completion() const { return state_->last_submission; }

bool cuda_staging_s::submit_transfer(const completion_s& dependency)
{
    auto& state = *state_;
    check_cuda(cudaSetDevice(state.cuda_device_index), "select CUDA transfer device");
    if (state.phase == state_s::phase_e::idle && !start_transfer(dependency)) {
        return false;
    }

    // Do not queue a Vulkan wait until CUDA has actually signalled it. A stalled
    // CUDA copy must not block unrelated render submissions on the shared queue.
    if (state.phase == state_s::phase_e::copying) {
        const auto ready = cudaEventQuery(state.copy_completed_event);
        if (ready == cudaErrorNotReady) {
            return false;
        }

        check_cuda(ready, "CUDA copy completion");
        state.phase = state_s::phase_e::acquiring;
    }

    // Return ownership before publishing either GPU input or readable host output.
    if (state.phase == state_s::phase_e::acquiring) {
        auto record =
            state.recording_context != nullptr ? state.recording_context->try_record() : state.device.try_record();
        if (!record) {
            return false;
        }

        record_ownership_transfer(*record, ownership_operation_e::acquire_from_cuda);
        if (state.direction == transfer_direction_e::cpu_to_gpu) {
            if (state.frame.buffer()) {
                record->copy(state.shared_transfer_buffer, state.frame.buffer(), state.frame.buffer().size());
            } else {
                record->upload(
                    state.shared_transfer_buffer, *state.frame.texture(), state.plan.host_layout.row_stride_bytes);
                record->generate_mip_maps(*state.frame.texture());
            }
        }

        state.last_submission = submit(*record, ownership_operation_e::acquire_from_cuda);
        state.phase           = state_s::phase_e::submitted;
    }

    return true;
}

bool cuda_staging_s::transfer_ready()
{
    auto& state = *state_;
    if (state.phase == state_s::phase_e::idle) {
        return true;
    }

    if (state.phase != state_s::phase_e::submitted || !state.last_submission.ready()) {
        return false;
    }

    state.phase = state_s::phase_e::idle;
    return true;
}
} // namespace miximus::gpu::transfer::detail
