#include "cuda_transfer.hpp"

#include "gpu/detail/device.hpp"
#include "logger/logger.hpp"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstring>
#include <cuda_runtime_api.h>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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

struct cuda_transfer_s::state_s
{
    device_s&                                    device;
    recording_context_s*                         recording_context{};
    std::shared_ptr<gpu::detail::device_state_s> owner;
    texture_frame_s&                             frame;
    texture_transfer_plan_s                      plan;
    transfer_direction_e                         direction;
    int                                          cuda_device_index{-1};

    cudaExternalMemory_t imported_memory{};
    void*                device_address{};
    cudaMipmappedArray_t image_mipmaps{};
    cudaArray_t          image_array{};
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

        if (image_mipmaps != nullptr) {
            (void)cudaFreeMipmappedArray(image_mipmaps);
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

        // The frame owns the native allocation and outlives this CUDA registration.
    }
};

cuda_transfer_s::cuda_transfer_s(device_s&                      device,
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
    import_frame_memory();
    create_external_semaphores();
}

int find_cuda_device(std::span<const uint8_t, 16> device_uuid, std::vector<std::string>& missing_support)
{
    int        count{};
    const auto enumeration = cudaGetDeviceCount(&count);
    if (enumeration != cudaSuccess && enumeration != cudaErrorNoDevice) {
        missing_support.push_back(std::format("CUDA device enumeration: {}", cudaGetErrorString(enumeration)));
        return -1;
    }

    std::vector<std::string> property_failures;
    for (int candidate = 0; candidate < count; ++candidate) {
        cudaDeviceProp properties{};
        const auto     status = cudaGetDeviceProperties(&properties, candidate);
        if (status != cudaSuccess) {
            property_failures.push_back(
                std::format("CUDA device {} properties: {}", candidate, cudaGetErrorString(status)));
            continue;
        }
        if (std::memcmp(properties.uuid.bytes, device_uuid.data(), device_uuid.size()) != 0) {
            continue;
        }

        // Confirm runtime initialization once, before selecting the backend. An
        // installed toolkit alone does not imply a usable driver/device pair.
        auto initialized = cudaSetDevice(candidate);
        if (initialized == cudaSuccess) {
            initialized = cudaFree(nullptr);
        }
        if (initialized != cudaSuccess) {
            missing_support.push_back(std::format("CUDA runtime initialization: {}", cudaGetErrorString(initialized)));
            return -1;
        }
        return candidate;
    }
    if (property_failures.empty()) {
        missing_support.emplace_back("CUDA is not supported on this device");
    } else {
        missing_support = std::move(property_failures);
    }
    return -1;
}

std::vector<std::string> qualify_cuda_transfers(device_s& device)
{
    std::vector<std::string> missing_support;

    // Use the production frame allocator and CUDA registrations, including image
    // mip chains, padded word buffers, SDK alignment and both host-access policies.
    // Enumerating the host formats keeps new formats inside this startup gate.
    for (const auto format : magic_enum::enum_values<host_pixel_format_e>()) {
        for (const auto sampling : magic_enum::enum_values<sampling_e>()) {
            try {
                const host_frame_layout_s layout{
                    .image_dimensions        = {48, 4},
                    .pixel_format            = format,
                    .row_stride_bytes        = 256,
                    .buffer_size_bytes       = 1024,
                    .address_alignment_bytes = 4096,
                };
                texture_frame_s frame(device, layout, sampling);
                auto            plan = make_texture_transfer_plan(layout);
                {
                    cuda_transfer_s upload(device, plan, transfer_direction_e::cpu_to_gpu, frame, nullptr);
                }
                plan.host_layout.memory_access = host_memory_access_e::read_write;
                cuda_transfer_s readback(device, plan, transfer_direction_e::gpu_to_cpu, frame, nullptr);
            } catch (const std::exception& error) {
                const auto representation = format == host_pixel_format_e::v210        ? "v210 word buffer"
                                            : sampling == sampling_e::mipmapped_linear ? "RGBA8 image with mipmaps"
                                                                                       : "RGBA8 image without mipmaps";
                const auto failure        = std::format("{}: {}", representation, error.what());
                // Channel-order aliases use the same native resource. Report each
                // missing capability once instead of repeating it for every alias.
                if (std::ranges::find(missing_support, failure) == missing_support.end()) {
                    missing_support.push_back(failure);
                }
            }
        }
    }
    return missing_support;
}

void cuda_transfer_s::select_cuda_device()
{
    auto& state             = *state_;
    state.cuda_device_index = state.owner->cuda_device_index;
    if (state.cuda_device_index < 0) {
        throw std::logic_error("CUDA transfer created without a matching CUDA device");
    }
    check_cuda(cudaSetDevice(state.cuda_device_index), "select CUDA device");
}

void cuda_transfer_s::validate_external_resources()
{
    auto& state = *state_;

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

void cuda_transfer_s::allocate_host_memory()
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

void cuda_transfer_s::import_frame_memory()
{
    auto& state = *state_;

    // Import the resource the shader actually samples or writes. No second device
    // buffer or device-to-device copy is needed for either transfer representation.
    VkDeviceMemory memory{};
    if (state.frame.buffer()) {
        const auto& resource          = state.frame.buffer().state_;
        memory                        = resource->external_memory;
        state.device_allocation_bytes = resource->info.bytes;
    } else {
        const auto& resource          = state.frame.texture()->state_;
        memory                        = resource->external_memory;
        state.device_allocation_bytes = resource->external_allocation_bytes;
        if (resource->format != format_e::rgba_unorm8) {
            throw std::invalid_argument("CUDA byte transfers require raw RGBA8 storage and shader component mapping");
        }
    }
    if (memory == VK_NULL_HANDLE) {
        throw std::invalid_argument("CUDA transfer frame was not allocated for external access");
    }

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory     = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd{-1};
    check(state.owner->vk.vkGetMemoryFdKHR(state.owner->device, &fd_info, &fd), "export Vulkan allocation FD");

    cudaExternalMemoryHandleDesc import{};
    import.type              = cudaExternalMemoryHandleTypeOpaqueFd;
    import.handle.fd         = fd;
    import.size              = state.device_allocation_bytes;
    import.flags             = cudaExternalMemoryDedicated;
    const auto import_result = cudaImportExternalMemory(&state.imported_memory, &import);
    if (import_result != cudaSuccess) {
        (void)::close(fd); // Successful CUDA import owns the FD.
    }

    check_cuda(import_result, "import Vulkan allocation into CUDA");

    if (state.frame.buffer()) {
        cudaExternalMemoryBufferDesc mapped{};
        mapped.size = state.frame.buffer().size();
        check_cuda(cudaExternalMemoryGetMappedBuffer(&state.device_address, state.imported_memory, &mapped),
                   "map CUDA packed frame buffer");
    } else {
        const auto&                          image = *state.frame.texture();
        cudaExternalMemoryMipmappedArrayDesc mapped{};
        mapped.formatDesc = {8, 8, 8, 8, cudaChannelFormatKindUnsigned};
        mapped.extent     = {image.extent().width, image.extent().height, 0};
        mapped.flags      = cudaArrayColorAttachment;
        mapped.numLevels  = image.mip_levels();
        check_cuda(cudaExternalMemoryGetMappedMipmappedArray(&state.image_mipmaps, state.imported_memory, &mapped),
                   "map CUDA raw RGBA8 frame image");
        check_cuda(cudaGetMipmappedArrayLevel(&state.image_array, state.image_mipmaps, 0), "map CUDA frame base level");
    }
}

void cuda_transfer_s::create_external_semaphores()
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

cuda_transfer_s::~cuda_transfer_s() = default;

void*  cuda_transfer_s::host_memory() const noexcept { return state_->host_address; }
size_t cuda_transfer_s::host_buffer_size_bytes() const noexcept { return state_->plan.host_layout.buffer_size_bytes; }
size_t cuda_transfer_s::allocation_bytes() const noexcept
{
    const auto& state       = *state_;
    const auto  frame_bytes = state.frame.buffer()
                                  ? state.frame.buffer().size()
                                  : texture_s::estimate_storage_byte_size(state.frame.texture()->dimensions(),
                                                                         format_e::rgba_unorm8,
                                                                         state.frame.texture()->mip_levels() > 1
                                                                              ? sampling_e::mipmapped_linear
                                                                              : sampling_e::linear);
    // The service accounts for frame storage separately; include only its allocation padding here.
    return state.host_allocation_bytes + state.device_allocation_bytes - frame_bytes;
}

void cuda_transfer_s::prepare_host_write()
{
    if (state_->phase != state_s::phase_e::idle) {
        throw std::logic_error("CUDA host write before transfer completion");
    }
}

void cuda_transfer_s::record_ownership_transfer(recording_s& record, ownership_operation_e operation)
{
    auto&      state         = *state_;
    const bool releasing     = operation == ownership_operation_e::release_to_cuda;
    const auto source_family = releasing ? state.owner->queue_family : VK_QUEUE_FAMILY_EXTERNAL;
    const auto target_family = releasing ? VK_QUEUE_FAMILY_EXTERNAL : state.owner->queue_family;
    const auto source_stage  = releasing ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
    const auto target_stage  = releasing ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const auto source_access =
        releasing ? VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_NONE;
    const auto target_access =
        releasing ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    VkBufferMemoryBarrier2 buffer_barrier{};
    VkImageMemoryBarrier2  image_barrier{};
    if (state.frame.buffer()) {
        const auto& buffer = state.frame.buffer().state_;
        record.state_->retain(buffer);
        buffer_barrier.sType                = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buffer_barrier.srcStageMask         = source_stage;
        buffer_barrier.srcAccessMask        = source_access;
        buffer_barrier.dstStageMask         = target_stage;
        buffer_barrier.dstAccessMask        = target_access;
        buffer_barrier.srcQueueFamilyIndex  = source_family;
        buffer_barrier.dstQueueFamilyIndex  = target_family;
        buffer_barrier.buffer               = buffer->buffer;
        buffer_barrier.size                 = VK_WHOLE_SIZE;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers    = &buffer_barrier;
    } else {
        const auto& image = state.frame.texture()->state_;
        record.state_->retain(image);
        if (releasing) {
            // CUDA accesses optimal-tiled images in GENERAL. Resolve prior layouts
            // through the normal submission prologue before releasing ownership.
            for (uint32_t mip = 0; mip < image->mip_levels; ++mip) {
                record.state_->transition(image,
                                          VK_IMAGE_LAYOUT_GENERAL,
                                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                          mip);
            }
        } else {
            // The matching release left every level in GENERAL. Seed local layouts
            // without a prologue: Vulkan must acquire ownership before touching it.
            record.state_->layouts.emplace(image.get(), std::vector(image->mip_levels, VK_IMAGE_LAYOUT_GENERAL));
            if (state.direction == transfer_direction_e::cpu_to_gpu) {
                image->content_version.fetch_add(1, std::memory_order_relaxed);
                record.state_->mip_dirty[image.get()] = true;
            }
        }
        image_barrier.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        image_barrier.srcStageMask         = source_stage;
        image_barrier.srcAccessMask        = source_access;
        image_barrier.dstStageMask         = target_stage;
        image_barrier.dstAccessMask        = target_access;
        image_barrier.srcQueueFamilyIndex  = source_family;
        image_barrier.dstQueueFamilyIndex  = target_family;
        image_barrier.oldLayout            = VK_IMAGE_LAYOUT_GENERAL;
        image_barrier.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
        image_barrier.image                = image->image;
        image_barrier.subresourceRange     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image->mip_levels, 0, 1};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &image_barrier;
    }
    state.owner->vk.vkCmdPipelineBarrier2(record.state_->arena->commands, &dependency);
}

completion_s cuda_transfer_s::submit(recording_s& record, ownership_operation_e operation)
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

bool cuda_transfer_s::start_transfer(const completion_s& dependency)
{
    auto& state = *state_;
    auto  record =
        state.recording_context != nullptr ? state.recording_context->try_record() : state.device.try_record();
    if (!record) {
        return false;
    }

    record->wait_for(dependency);

    record_ownership_transfer(*record, ownership_operation_e::release_to_cuda);
    state.last_submission = submit(*record, ownership_operation_e::release_to_cuda);

    cudaExternalSemaphoreWaitParams wait{};
    check_cuda(cudaWaitExternalSemaphoresAsync(&state.cuda_wait, &wait, 1, state.stream),
               "CUDA wait for Vulkan release");
    const auto uploading = state.direction == transfer_direction_e::cpu_to_gpu;
    if (state.frame.buffer()) {
        check_cuda(cudaMemcpyAsync(uploading ? state.device_address : state.host_address,
                                   uploading ? state.host_address : state.device_address,
                                   state.frame.buffer().size(),
                                   uploading ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost,
                                   state.stream),
                   "CUDA direct packed frame copy");
    } else {
        const auto& layout    = state.plan.host_layout;
        const auto  row_bytes = static_cast<size_t>(layout.image_dimensions.x) * state.plan.storage_bytes_per_texel;
        const auto  rows      = static_cast<size_t>(layout.image_dimensions.y);
        if (uploading) {
            check_cuda(cudaMemcpy2DToArrayAsync(state.image_array,
                                                0,
                                                0,
                                                state.host_address,
                                                layout.row_stride_bytes,
                                                row_bytes,
                                                rows,
                                                cudaMemcpyHostToDevice,
                                                state.stream),
                       "CUDA direct host-to-image copy");
        } else {
            check_cuda(cudaMemcpy2DFromArrayAsync(state.host_address,
                                                  layout.row_stride_bytes,
                                                  state.image_array,
                                                  0,
                                                  0,
                                                  row_bytes,
                                                  rows,
                                                  cudaMemcpyDeviceToHost,
                                                  state.stream),
                       "CUDA direct image-to-host copy");
        }
    }

    cudaExternalSemaphoreSignalParams signal{};
    check_cuda(cudaSignalExternalSemaphoresAsync(&state.cuda_signal, &signal, 1, state.stream),
               "CUDA signal Vulkan acquire");
    check_cuda(cudaEventRecord(state.copy_completed_event, state.stream), "CUDA copy completion event");
    state.phase = state_s::phase_e::copying;
    return true;
}

completion_s cuda_transfer_s::completion() const { return state_->last_submission; }

bool cuda_transfer_s::submit_transfer(const completion_s& dependency)
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
        if (state.direction == transfer_direction_e::cpu_to_gpu && state.frame.texture()) {
            record->generate_mip_maps(*state.frame.texture());
        }

        state.last_submission = submit(*record, ownership_operation_e::acquire_from_cuda);
        state.phase           = state_s::phase_e::submitted;
    }

    return true;
}

bool cuda_transfer_s::transfer_ready()
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
