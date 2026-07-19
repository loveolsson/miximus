#pragma once

#include "backend.hpp"
#include "gpu/glad.hpp"
#include "gpu/transfer/texture_transfer.hpp"

#include <cuda_gl_interop.h>
#include <cuda_runtime_api.h>

namespace miximus::gpu::transfer::detail {

class cuda_transfer_s : public backend_i
{
    cudaStream_t          stream_{nullptr};
    cudaEvent_t           completion_{nullptr};
    GLuint                buffer_{0};
    cudaGraphicsResource* buffer_resource_{nullptr};
    cudaGraphicsResource* texture_resource_{nullptr};
    GLuint                registered_texture_{};
    size_t                row_stride_{};
    GLint                 row_length_{};
    bool                  direct_image_{};
    bool                  pending_{false};

    static bool initialized_;
    static bool supported_;
    static int  device_;

    bool ensure_buffer();
    bool copy_host_to_buffer();
    bool copy_buffer_to_host();
    bool copy_host_to_texture(texture_s* texture);
    bool copy_texture_to_host(texture_s* texture);
    bool ensure_texture_resource(texture_s* texture);

    bool register_texture_impl(texture_s* texture) final;
    bool unregister_texture_impl(texture_s* texture) final;

  public:
    cuda_transfer_s(const texture_transfer_requirements_s& requirements, direction_e dir, bool direct_image);
    ~cuda_transfer_s() override;

    bool transfer() final;
    bool wait_for_completion() final;

    static bool initialize_context();
    static void shutdown_context();
};

} // namespace miximus::gpu::transfer::detail
