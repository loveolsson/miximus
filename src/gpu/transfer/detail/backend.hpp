#pragma once
#include "gpu/texture_fwd.hpp"

#include <cassert>
#include <cstddef>

namespace miximus::gpu::transfer::detail {

constexpr size_t TRANSFER_ALIGNMENT = 16;

class backend_i
{
  public:
    enum class direction_e
    {
        gpu_to_cpu,
        cpu_to_gpu,
    };

    virtual ~backend_i();

    backend_i(const backend_i&)            = delete;
    backend_i(backend_i&&)                 = delete;
    backend_i& operator=(const backend_i&) = delete;
    backend_i& operator=(backend_i&&)      = delete;

    size_t size() const { return size_; }
    void*  data() const { return data_; }

    bool bind_texture(texture_s* texture);
    bool unbind_texture();
    bool begin_texture_use();
    bool end_texture_use();

    virtual bool transfer()            = 0;
    virtual bool wait_for_completion() = 0;

  protected:
    backend_i(size_t size, direction_e direction);

    texture_s* texture() const
    {
        assert(texture_ != nullptr);
        return texture_;
    }

    void allocate_data();
    void free_data();

    virtual bool register_texture_impl(texture_s*) { return true; }
    virtual bool unregister_texture_impl(texture_s*) { return true; }
    virtual bool begin_texture_use_impl(texture_s*) { return true; }
    virtual bool end_texture_use_impl(texture_s*) { return true; }

    const size_t      size_;
    const direction_e direction_;
    void*             data_{};

  private:
    texture_s* texture_{};
};

} // namespace miximus::gpu::transfer::detail
