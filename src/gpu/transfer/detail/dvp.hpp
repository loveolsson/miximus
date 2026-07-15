#pragma once
#include "gpu/glad.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/transfer.hpp"

#include <DVPAPI.h>
#include <dvpapi_gl.h>
#include <memory>
#include <mutex>
#include <unordered_map>

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
// The static register_texture / unregister_texture / begin_texture_use /
// end_texture_use calls match the hooks in transfer_i and the decklink allocator.

class dvp_transfer_s : public transfer_i
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

    // Texture DVP handle used in the last perform_transfer call; needed by wait_for_copy.
    DVPBufferHandle last_texture_handle_{0};

    // Per-GL-context DVP initialization state (shared across all instances).
    static bool     ctx_initialized_;
    static bool     dvp_supported_;
    static uint32_t buf_addr_align_;
    static uint32_t buf_gpu_stride_align_;
    static uint32_t sem_addr_align_;
    static uint32_t sem_alloc_size_;
    static uint32_t sem_payload_offset_;
    static uint32_t sem_payload_size_;

    // Map from texture_s* → DVPBufferHandle, guarded by a mutex.
    static std::mutex                                      texture_map_mutex_;
    static std::unordered_map<texture_s*, DVPBufferHandle> texture_handles_;

    static DVPBufferHandle lookup_texture(texture_s* texture);

    void perform_dma(DVPBufferHandle src, DVPBufferHandle dst, uint32_t height);

  public:
    dvp_transfer_s(size_t size, direction_e dir);
    ~dvp_transfer_s();

    type_e type() const final { return type_e::dvp; }
    bool   perform_copy() final { return true; } // no-op: ptr_ IS the DMA source/dest
    bool   perform_transfer(texture_s* texture) final;
    bool   perform_transfer(framebuffer_s* fb) final;
    bool   wait_for_copy() final;

    // Called once during app initialization with the root GL context current.
    static bool initialize_context();
    // Called during app shutdown with the same root GL context current.
    static void shutdown_context();

    static bool register_texture_dvp(texture_s* texture);
    static bool unregister_texture_dvp(texture_s* texture);
    static bool begin_texture_use_dvp(texture_s* texture); // dvpMapBufferWaitAPI
    static bool end_texture_use_dvp(texture_s* texture);   // dvpMapBufferEndAPI
};

} // namespace miximus::gpu::transfer::detail
