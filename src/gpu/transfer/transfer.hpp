#pragma once
#include "gpu/texture_fwd.hpp"

#include <memory>

namespace miximus::gpu { namespace transfer {

constexpr size_t ALIGNMENT = 16;

class transfer_i
{
  public:
    enum class type_e
    {
        basic,
        persistent,
        cuda,
        dvp,
    };

    enum class direction_e
    {
        gpu_to_cpu,
        cpu_to_gpu,
    };

    virtual ~transfer_i() = default;

    size_t         size() const { return size_; }
    virtual type_e type() const = 0;

    void*        ptr() const { return ptr_; }
    virtual bool perform_copy()               = 0;
    virtual bool perform_transfer(texture_s*) = 0;
    virtual bool wait_for_copy()              = 0;

    static type_e get_prefered_type();
    // Called during application startup with the root GL context current. It
    // selects DVP, CUDA/OpenGL interop, or the persistent-PBO fallback.
    static void initialize_preferred_type();
    // Called during application shutdown with the root GL context current, after
    // all transfers and registered textures have been destroyed.
    static void shutdown();
    static bool register_texture(type_e type, gpu::texture_s* texture);
    static bool unregister_texture(type_e type, gpu::texture_s* texture);
    static bool begin_texture_use(type_e type, gpu::texture_s* texture);
    static bool end_texture_use(type_e type, gpu::texture_s* texture);

    static std::unique_ptr<transfer_i> create_transfer(type_e type, size_t size, direction_e dir);

  protected:
    transfer_i(size_t size, direction_e direction);

    void allocate_ptr();
    void free_ptr();

    const size_t      size_;
    const direction_e direction_;
    void*             ptr_;
};
}} // namespace miximus::gpu::transfer
