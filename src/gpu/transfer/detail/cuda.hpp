#pragma once

#include "gpu/glad.hpp"
#include "gpu/transfer/transfer.hpp"

#include <cuda_gl_interop.h>
#include <cuda_runtime_api.h>

namespace miximus::gpu::transfer::detail {

class cuda_transfer_s : public transfer_i
{
    cudaStream_t          stream_{nullptr};
    cudaEvent_t           completion_{nullptr};
    GLuint                buffer_{0};
    cudaGraphicsResource* buffer_resource_{nullptr};
    bool                  pending_{false};

    static bool initialized_;
    static bool supported_;
    static int  device_;

    bool ensure_buffer();
    bool copy_host_to_buffer();
    bool copy_buffer_to_host();

  public:
    cuda_transfer_s(size_t size, direction_e dir);
    ~cuda_transfer_s() override;

    type_e type() const final { return type_e::cuda; }

    bool perform_copy() final { return true; }
    bool perform_transfer(texture_s* texture) final;
    bool perform_transfer(framebuffer_s* framebuffer) final;
    bool wait_for_copy() final;

    static bool initialize_context();
    static void shutdown_context();
};

} // namespace miximus::gpu::transfer::detail
