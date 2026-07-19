#pragma once
#include "backend.hpp"
#include "gpu/glad.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/texture_transfer.hpp"

#include <DVPAPI.h>
#include <dvpapi_gl.h>
#include <memory>

namespace miximus::gpu::transfer::detail {

// DVP (NVIDIA GPU Direct for Video) transfer.
//
// Uses NVIDIA's DVP DMA engine to move data between page-locked system memory
// and GPU textures without any CPU involvement in the copy itself.
//
// Overhead per transfer is roughly 5-15 µs for DMA setup + semaphore signaling.
// At 720p BGRA (3.7 MB) and above this is always faster than glReadPixels/
// glTexSubImage2D via PBO. Below ~512 KB a plain PBO may be faster; at all video
// production resolutions DVP is the right choice.
//
// Ownership model (MapBuffer pairs):
//   API  → DVP  : dvpMapBufferEndAPI  (outside begin/end) → dvpMapBufferWaitDVP (inside)
//   DVP  → API  : dvpMapBufferEndDVP  (inside begin/end)  → dvpMapBufferWaitAPI (outside)
//
class dvp_transfer_s : public backend_i
{
    // Per-buffer DVP handles
    DVPBufferHandle sysmem_handle_{0};

    // Two semaphores per buffer (matches the DeckLink example's SyncInfo pattern):
    //   ext_sync: signaled by CPU/API to tell DVP data is ready / texture is accessible
    //   gpu_sync: signaled by DVP to tell CPU/API the DMA is complete
    struct semaphore_s
    {
        uint32_t*           mem{nullptr};
        uint32_t*           mem_unaligned{nullptr};
        volatile uint32_t   release_value{0};
        volatile uint32_t   acquire_value{0};
        DVPSyncObjectHandle dvp_handle{0};

        semaphore_s() = default;
        ~semaphore_s();
        semaphore_s(const semaphore_s&)            = delete;
        semaphore_s& operator=(const semaphore_s&) = delete;

        void init(uint32_t alloc_size, uint32_t addr_align);
        void destroy();
    };

    semaphore_s ext_sync_; // CPU signals → DVP waits
    semaphore_s gpu_sync_; // DVP signals → CPU waits

    DVPBufferHandle texture_handle_{0};

    // Per-GL-context DVP initialization state (shared across all instances).
    static bool     ctx_initialized_;
    static bool     dvp_supported_;
    static uint32_t buf_addr_align_;
    static uint32_t buf_gpu_stride_align_;
    static uint32_t sem_addr_align_;
    static uint32_t sem_alloc_size_;
    static uint32_t sem_payload_offset_;
    static uint32_t sem_payload_size_;

    bool perform_dma(DVPBufferHandle src, DVPBufferHandle dst, uint32_t height);

    bool register_texture_impl(texture_s* texture) final;
    bool unregister_texture_impl(texture_s* texture) final;
    bool begin_texture_use_impl(texture_s* texture) final;
    bool end_texture_use_impl(texture_s* texture) final;

  public:
    dvp_transfer_s(const texture_transfer_requirements_s& requirements, direction_e dir);
    ~dvp_transfer_s();

    bool transfer() final;
    bool wait_for_completion() final;

    // Called once during app initialization with the root GL context current.
    static bool initialize_context();
    // Called during app shutdown with the same root GL context current.
    static void shutdown_context();
    static bool supports(const texture_transfer_requirements_s& requirements);
};

} // namespace miximus::gpu::transfer::detail
