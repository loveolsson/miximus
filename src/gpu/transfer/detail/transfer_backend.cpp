#include "transfer_backend.hpp"
#ifdef MIXIMUS_HAS_CUDA
#include "cuda_transfer.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <format>
#include <stdexcept>

namespace miximus::gpu::transfer::detail {
namespace {

host_access_e host_access_for(const texture_transfer_plan_s& plan, transfer_direction_e direction)
{
    // Readback and SDK read/modify/write leases need cached readable host memory.
    if (direction == transfer_direction_e::gpu_to_cpu ||
        plan.host_layout.memory_access == host_memory_access_e::read_write) {
        return host_access_e::read_write;
    }

    return host_access_e::sequential_write;
}

class vulkan_staging_s final : public transfer_backend_i
{
    device_s&               device_;
    recording_context_s*    recording_context_;
    texture_frame_s&        frame_;
    texture_transfer_plan_s plan_;
    transfer_direction_e    direction_;
    buffer_s                host_;
    completion_s            completion_;
    void*                   address_{};

  public:
    vulkan_staging_s(device_s&                      device,
                     const texture_transfer_plan_s& plan,
                     transfer_direction_e           direction,
                     texture_frame_s&               frame,
                     recording_context_s*           recording_context)
        : device_(device)
        , recording_context_(recording_context)
        , frame_(frame)
        , plan_(plan)
        , direction_(direction)
        , host_(device.create_buffer(plan.host_layout.buffer_size_bytes,
                                     host_access_for(plan, direction),
                                     plan.host_layout.address_alignment_bytes))
    {
        auto bytes = host_.writable_bytes();
        std::ranges::fill(bytes, std::byte{});
        address_ = bytes.data();
    }

    void*            host_memory() const noexcept override { return address_; }
    size_t           host_buffer_size_bytes() const noexcept override { return plan_.host_layout.buffer_size_bytes; }
    size_t           allocation_bytes() const override { return host_.allocation_info().bytes; }
    std::string_view backend_name() const noexcept override { return "vulkan-staging"; }
    void             prepare_host_write() override { (void)host_.writable_bytes(); }
    completion_s     completion() const override { return completion_; }
    bool             submit_transfer(const completion_s& dependency) override
    {
        auto record = recording_context_ != nullptr ? recording_context_->try_record() : device_.try_record();
        if (!record) {
            return false;
        }

        record->wait_for(dependency);

        if (frame_.buffer()) {
            if (direction_ == transfer_direction_e::cpu_to_gpu) {
                record->copy(host_, frame_.buffer(), frame_.buffer().size());
            } else {
                record->copy(frame_.buffer(), host_, frame_.buffer().size());
            }
        } else if (direction_ == transfer_direction_e::cpu_to_gpu) {
            record->upload(host_, *frame_.texture(), plan_.host_layout.row_stride_bytes);
            record->generate_mip_maps(*frame_.texture());
        } else {
            record->readback(*frame_.texture(), host_, plan_.host_layout.row_stride_bytes);
        }

        completion_ = record->submit();
        return true;
    }

    bool transfer_ready() override
    {
        if (!completion_.ready()) {
            return false;
        }

        // Completion precedes cache invalidation and publication of readable bytes.
        if (direction_ == transfer_direction_e::gpu_to_cpu) {
            (void)host_.readable_bytes();
        }

        return true;
    }
};
} // namespace

std::unique_ptr<transfer_backend_i> create_transfer_backend(device_s&                      device,
                                                            const texture_transfer_plan_s& plan,
                                                            transfer_direction_e           direction,
                                                            texture_frame_s&               frame,
                                                            recording_context_s*           recording_context)
{
    std::unique_ptr<transfer_backend_i> backend;
    if (device.uses_cuda_transfers()) {
        try {
#ifdef MIXIMUS_HAS_CUDA
            backend = std::make_unique<cuda_transfer_s>(device, plan, direction, frame, recording_context);
#else
            throw std::runtime_error("CUDA/Vulkan support was not compiled in (requires Linux and the CUDA toolkit)");
#endif
        } catch (const std::exception& error) {
            throw std::runtime_error(std::format("CUDA required; refusing Vulkan fallback: {}", error.what()));
        }
    } else {
        backend = std::make_unique<vulkan_staging_s>(device, plan, direction, frame, recording_context);
    }

    // Enforce the SDK-facing contract before any lease can publish the address.
    if (backend->host_memory() == nullptr || backend->host_buffer_size_bytes() != plan.host_layout.buffer_size_bytes ||
        reinterpret_cast<uintptr_t>(backend->host_memory()) % plan.host_layout.address_alignment_bytes != 0) {
        throw std::runtime_error("Transfer backend does not satisfy the negotiated host-memory layout");
    }

    return backend;
}
} // namespace miximus::gpu::transfer::detail
