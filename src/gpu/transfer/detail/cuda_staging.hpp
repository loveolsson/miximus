#pragma once

#include "gpu/device.hpp"
#include "gpu/texture_frame.hpp"
#include "transfer_backend.hpp"

namespace miximus::gpu::transfer::detail {

// Linux CUDA/Vulkan external-buffer interoperability. Native handles stay in the implementation.
class cuda_staging_s final : public transfer_backend_i
{
    enum class ownership_operation_e
    {
        release_to_cuda,
        acquire_from_cuda,
    };

    struct state_s;
    std::unique_ptr<state_s> state_;
    completion_s             submit(recording_s& record, ownership_operation_e operation);
    void                     record_ownership_transfer(recording_s& record, ownership_operation_e operation);

    bool start_transfer(const completion_s& dependency);

    void select_cuda_device();
    void validate_external_resources();
    void allocate_host_memory();
    void import_transfer_buffer();
    void create_external_semaphores();

  public:
    cuda_staging_s(device_s&                      device,
                   const texture_transfer_plan_s& plan,
                   transfer_direction_e           direction,
                   texture_frame_s&               frame,
                   recording_context_s*           recording_context);
    ~cuda_staging_s() override;
    void*            host_memory() const noexcept override;
    size_t           host_buffer_size_bytes() const noexcept override;
    size_t           allocation_bytes() const noexcept override;
    std::string_view backend_name() const noexcept override { return "cuda-vulkan-buffer"; }
    void             prepare_host_write() override;
    bool             submit_transfer(const completion_s& dependency) override;
    completion_s     completion() const override;
    bool             transfer_ready() override;
};
} // namespace miximus::gpu::transfer::detail
