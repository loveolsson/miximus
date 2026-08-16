#pragma once
#include "gpu/texture_fwd.hpp"
#include "gpu/transfer/readback_component_mapping.hpp"

#include <cassert>
#include <cstddef>

namespace miximus::gpu::transfer::detail {

constexpr size_t HOST_MEMORY_ALIGNMENT_BYTES = 16;

class texture_transfer_backend_i
{
  public:
    enum class direction_e
    {
        gpu_to_cpu,
        cpu_to_gpu,
    };

    virtual ~texture_transfer_backend_i();

    texture_transfer_backend_i(const texture_transfer_backend_i&)            = delete;
    texture_transfer_backend_i(texture_transfer_backend_i&&)                 = delete;
    texture_transfer_backend_i& operator=(const texture_transfer_backend_i&) = delete;
    texture_transfer_backend_i& operator=(texture_transfer_backend_i&&)      = delete;

    size_t host_buffer_size_bytes() const noexcept { return host_buffer_size_bytes_; }
    void*  host_memory() const noexcept { return host_memory_; }

    bool register_texture(texture_s* texture);
    bool unregister_texture();
    bool acquire_texture_for_gl();
    bool release_texture_from_gl();

    virtual readback_component_mapping_e readback_component_mapping() const noexcept
    {
        return readback_component_mapping_e::identity;
    }

    virtual bool submit_transfer()              = 0;
    virtual bool wait_for_transfer_completion() = 0;

  protected:
    texture_transfer_backend_i(size_t host_buffer_size_bytes, direction_e direction);

    texture_s* texture() const noexcept
    {
        assert(texture_ != nullptr);
        return texture_;
    }

    void allocate_host_memory();
    void free_host_memory();

    virtual bool register_texture_impl(texture_s*) { return true; }
    virtual bool unregister_texture_impl(texture_s*) { return true; }
    virtual bool acquire_texture_for_gl_impl(texture_s*) { return true; }
    virtual bool release_texture_from_gl_impl(texture_s*) { return true; }

    const size_t      host_buffer_size_bytes_;
    const direction_e direction_;
    void*             host_memory_{};

  private:
    texture_s* texture_{};
};

} // namespace miximus::gpu::transfer::detail
