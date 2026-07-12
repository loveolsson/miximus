#include "dvp.hpp"

#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "logger/logger.hpp"

#include <cassert>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h> // VirtualLock / VirtualUnlock
#else
#include <GL/glx.h>   // glXGetCurrentContext — detect EGL vs GLX before touching DVP
#include <sys/mman.h> // mlock / munlock
#endif

#define DVP_CHECK(cmd)                                                                                                 \
    do {                                                                                                               \
        const DVPStatus dvp_status_ = (cmd);                                                                           \
        if (dvp_status_ != DVP_STATUS_OK) {                                                                            \
            getlog("gpu")->error("DVP call failed: " #cmd " status={}", static_cast<int>(dvp_status_));                \
        }                                                                                                              \
    } while (0)

namespace miximus::gpu::transfer::detail {

// Static members
bool     dvp_transfer_s::ctx_initialized_      = false;
bool     dvp_transfer_s::dvp_supported_        = false;
uint32_t dvp_transfer_s::buf_addr_align_       = 1;
uint32_t dvp_transfer_s::buf_gpu_stride_align_ = 1;
uint32_t dvp_transfer_s::sem_addr_align_       = 1;
uint32_t dvp_transfer_s::sem_alloc_size_       = sizeof(uint32_t);
uint32_t dvp_transfer_s::sem_payload_offset_   = 0;
uint32_t dvp_transfer_s::sem_payload_size_     = sizeof(uint32_t);

std::mutex                                      dvp_transfer_s::texture_map_mutex_;
std::unordered_map<texture_s*, DVPBufferHandle> dvp_transfer_s::texture_handles_;

// ─── semaphore_s ─────────────────────────────────────────────────────────────

void dvp_transfer_s::semaphore_s::init(uint32_t alloc_size, uint32_t addr_align)
{
    mem_unaligned = static_cast<volatile uint32_t*>(malloc(alloc_size + addr_align - 1));

    uintptr_t val = reinterpret_cast<uintptr_t>(mem_unaligned);
    val           = (val + addr_align - 1) & ~(static_cast<uintptr_t>(addr_align) - 1);
    mem           = reinterpret_cast<volatile uint32_t*>(val);

    mem[0]        = 0;
    release_value = 0;
    acquire_value = 0;

    DVPSyncObjectDesc desc{};
    desc.externalClientWaitFunc = nullptr;
    desc.sem                    = const_cast<uint32_t*>(mem);
    desc.flags                  = DVP_SYNC_OBJECT_FLAGS_USE_EVENTS;

    DVP_CHECK(dvpImportSyncObject(&desc, &dvp_handle));
}

void dvp_transfer_s::semaphore_s::destroy()
{
    if (dvp_handle != 0) {
        DVP_CHECK(dvpFreeSyncObject(dvp_handle));
        dvp_handle = 0;
    }
    if (mem_unaligned != nullptr) {
        free(const_cast<uint32_t*>(mem_unaligned));
        mem_unaligned = nullptr;
        mem           = nullptr;
    }
}

dvp_transfer_s::semaphore_s::~semaphore_s() { destroy(); }

// ─── Context initialization ──────────────────────────────────────────────────

bool dvp_transfer_s::initialize_context()
{
    if (ctx_initialized_) {
        return dvp_supported_;
    }
    ctx_initialized_ = true; // mark attempted regardless of outcome

#ifndef _WIN32
    // DVP's GL integration internally calls glXMakeContextCurrent and requires a
    // GLX context. If the application is using EGL (as this project does via GLFW's
    // EGL backend), glXGetCurrentContext() returns NULL and DVP will fail with
    // X_GLXMakeContextCurrent BadAccess. Detect this early and skip DVP entirely.
    if (glXGetCurrentContext() == nullptr) {
        getlog("gpu")->info("Transfer: DVP skipped — EGL context detected (DVP requires GLX). "
                            "Using persistent PBO transfers.");
        return false;
    }
#endif

    DVPStatus status = dvpInitGLContext(DVP_DEVICE_FLAGS_SHARE_APP_CONTEXT);
    if (status != DVP_STATUS_OK) {
        getlog("gpu")->warn("DVP init failed (status={}) — falling back to persistent PBO transfers. "
                            "DVP requires a native NVIDIA X11 server with GLX_NV_gpu_affinity; "
                            "XWayland does not expose this extension. "
                            "Log in with an Xorg session (not Wayland) to enable DVP.",
                            static_cast<int>(status));
        return false;
    }

    status = dvpGetRequiredConstantsGLCtx(&buf_addr_align_,
                                          &buf_gpu_stride_align_,
                                          &sem_addr_align_,
                                          &sem_alloc_size_,
                                          &sem_payload_offset_,
                                          &sem_payload_size_);
    if (status != DVP_STATUS_OK) {
        getlog("gpu")->warn("dvpGetRequiredConstantsGLCtx failed (status={}) — falling back to persistent.",
                            static_cast<int>(status));
        dvpCloseGLContext();
        return false;
    }

    dvp_supported_ = true;
    getlog("gpu")->info("DVP GL context initialised (buf_align={}, sem_align={})", buf_addr_align_, sem_addr_align_);
    return true;
}

void dvp_transfer_s::shutdown_context()
{
    if (!ctx_initialized_) {
        return;
    }

    if (dvp_supported_) {
        {
            const std::lock_guard lock(texture_map_mutex_);
            if (!texture_handles_.empty()) {
                getlog("gpu")->error("DVP shutdown with {} registered texture(s); releasing them",
                                     texture_handles_.size());
                for (const auto& [texture, handle] : texture_handles_) {
                    (void)texture;
                    DVP_CHECK(dvpFreeBuffer(handle));
                }
                texture_handles_.clear();
            }
        }

        DVP_CHECK(dvpCloseGLContext());
    }

    dvp_supported_   = false;
    ctx_initialized_ = false;
}

DVPBufferHandle dvp_transfer_s::lookup_texture(texture_s* texture)
{
    const std::lock_guard lock(texture_map_mutex_);
    const auto            it = texture_handles_.find(texture);
    if (it == texture_handles_.end()) {
        getlog("gpu")->error("DVP: texture {} not registered", static_cast<void*>(texture));
        return 0;
    }
    return it->second;
}

bool dvp_transfer_s::register_texture_dvp(texture_s* texture)
{
    DVPBufferHandle handle = 0;
    DVP_CHECK(dvpCreateGPUTextureGL(texture->id(), &handle));

    {
        const std::lock_guard lock(texture_map_mutex_);
        texture_handles_[texture] = handle;
    }

    // Initialise ownership to "API/GL has the texture" so the first
    // dvpMapBufferWaitDVP inside perform_transfer works correctly.
    dvpMapBufferEndAPI(handle);

    return true;
}

bool dvp_transfer_s::unregister_texture_dvp(texture_s* texture)
{
    DVPBufferHandle handle = 0;
    {
        const std::lock_guard lock(texture_map_mutex_);
        const auto            it = texture_handles_.find(texture);
        if (it == texture_handles_.end()) {
            return false;
        }
        handle = it->second;
        texture_handles_.erase(it);
    }

    DVP_CHECK(dvpFreeBuffer(handle));
    return true;
}

bool dvp_transfer_s::begin_texture_use_dvp(texture_s* texture)
{
    const DVPBufferHandle handle = lookup_texture(texture);
    if (handle == 0) {
        return false;
    }
    // Tell GL it can use the texture: waits for DVP to have signalled EndDVP.
    dvpMapBufferWaitAPI(handle);
    return true;
}

bool dvp_transfer_s::end_texture_use_dvp(texture_s* texture)
{
    const DVPBufferHandle handle = lookup_texture(texture);
    if (handle == 0) {
        return false;
    }
    // Signal to DVP that GL is done with the texture.
    dvpMapBufferEndAPI(handle);
    return true;
}

// ─── Constructor / Destructor ────────────────────────────────────────────────

dvp_transfer_s::dvp_transfer_s(size_t size, direction_e dir)
    : transfer_i(size, dir)
{
    assert(dvp_supported_);

    // Allocate page-aligned, page-locked system memory.
    // buf_addr_align_ is the DVP requirement; use at least 4096 (page size).
    const size_t alignment = std::max(static_cast<size_t>(buf_addr_align_), static_cast<size_t>(4096));
    ptr_                   = aligned_alloc(alignment, size_);

    if (ptr_ == nullptr) {
        throw std::runtime_error("DVP: failed to allocate aligned sysmem");
    }

#ifdef _WIN32
    if (!VirtualLock(ptr_, size_)) {
        free(ptr_);
        ptr_ = nullptr;
        throw std::runtime_error("DVP: VirtualLock failed");
    }
#else
    if (mlock(ptr_, size_) != 0) {
        free(ptr_);
        ptr_ = nullptr;
        throw std::runtime_error("DVP: mlock failed — check RLIMIT_MEMLOCK");
    }
#endif

    // Register the sysmem buffer with DVP.
    DVPSysmemBufferDesc desc{};
    desc.width   = static_cast<uint32_t>(size_);
    desc.height  = 1;
    desc.stride  = static_cast<uint32_t>(size_);
    desc.size    = static_cast<uint32_t>(size_);
    desc.format  = DVP_BUFFER;
    desc.type    = DVP_UNSIGNED_BYTE;
    desc.bufAddr = ptr_;

    DVP_CHECK(dvpCreateBuffer(&desc, &sysmem_handle_));
    DVP_CHECK(dvpBindToGLCtx(sysmem_handle_));

    // Initialise semaphores.
    ext_sync_.init(sem_alloc_size_, sem_addr_align_);
    gpu_sync_.init(sem_alloc_size_, sem_addr_align_);
}

dvp_transfer_s::~dvp_transfer_s()
{
    if (sysmem_handle_ != 0) {
        DVP_CHECK(dvpUnbindFromGLCtx(sysmem_handle_));
        DVP_CHECK(dvpDestroyBuffer(sysmem_handle_));
        sysmem_handle_ = 0;
    }

    ext_sync_.destroy();
    gpu_sync_.destroy();

    if (ptr_ != nullptr) {
#ifdef _WIN32
        VirtualUnlock(ptr_, size_);
#else
        munlock(ptr_, size_);
#endif
        free(ptr_);
        ptr_ = nullptr;
    }
}

// ─── Core DMA helper ─────────────────────────────────────────────────────────

void dvp_transfer_s::perform_dma(DVPBufferHandle src, DVPBufferHandle dst, uint32_t height)
{
    gpu_sync_.release_value = gpu_sync_.release_value + 1;

    dvpBegin();
    dvpMapBufferWaitDVP(src);
    DVP_CHECK(dvpMemcpyLined(src,
                             ext_sync_.dvp_handle,
                             ext_sync_.acquire_value,
                             DVP_TIMEOUT_IGNORED,
                             dst,
                             gpu_sync_.dvp_handle,
                             gpu_sync_.release_value,
                             0,
                             height));
    dvpMapBufferEndDVP(src);
    dvpEnd();
}

// ─── transfer_i interface ────────────────────────────────────────────────────

bool dvp_transfer_s::perform_transfer(texture_s* texture)
{
    if (sysmem_handle_ == 0) {
        return false;
    }

    const DVPBufferHandle tex_handle = lookup_texture(texture);
    if (tex_handle == 0) {
        return false;
    }

    last_texture_handle_ = tex_handle;

    const auto dims = texture->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        // CPU wrote data into ptr_; DMA ptr_ → texture.
        // endTextureInUse (dvpMapBufferEndAPI) must have been called by the caller
        // before this point so DVP knows GL is done with the texture.
        perform_dma(sysmem_handle_, tex_handle, static_cast<uint32_t>(dims.y));
    } else {
        // GPU rendered to texture; DMA texture → ptr_.
        // Signal that GL is done writing to the texture, DVP can start reading.
        dvpMapBufferEndAPI(tex_handle);
        perform_dma(tex_handle, sysmem_handle_, static_cast<uint32_t>(dims.y));
    }

    return true;
}

bool dvp_transfer_s::perform_transfer(framebuffer_s* fb) { return perform_transfer(fb->texture()); }

bool dvp_transfer_s::wait_for_copy()
{
    if (direction_ == direction_e::cpu_to_gpu) {
        // For cpu_to_gpu: insert a GL wait so the GPU won't sample the texture
        // before DVP finishes writing it.  Must be called with GL context current.
        if (last_texture_handle_ != 0) {
            dvpMapBufferWaitAPI(last_texture_handle_);
        }
    } else {
        // For gpu_to_cpu: CPU blocks until DVP DMA into sysmem is complete.
        // No GL context required — dvpBegin/End and dvpSyncObjClientWaitComplete
        // are pure DVP / CPU operations.
        dvpBegin();
        DVP_CHECK(dvpSyncObjClientWaitComplete(gpu_sync_.dvp_handle, DVP_TIMEOUT_IGNORED));
        dvpEnd();
    }
    return true;
}

} // namespace miximus::gpu::transfer::detail
