#include "dvp.hpp"

#include "gpu/texture.hpp"
#include "logger/logger.hpp"

#include <cassert>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h> // VirtualLock / VirtualUnlock
#else
#include <GL/glx.h>   // glXGetCurrentContext — detect EGL vs GLX before touching DVP
#include <sys/mman.h> // mlock / munlock
#endif

namespace miximus::gpu::transfer::detail {

namespace {
void check_dvp_status(DVPStatus status, std::string_view command)
{
    if (status != DVP_STATUS_OK) {
        getlog("gpu")->error("DVP call failed: {} status={}", command, static_cast<int>(status));
    }
}
} // namespace

#define DVP_CHECK(cmd) check_dvp_status((cmd), #cmd)

// Static members
bool     dvp_transfer_s::ctx_initialized_      = false;
bool     dvp_transfer_s::dvp_supported_        = false;
uint32_t dvp_transfer_s::buf_addr_align_       = 1;
uint32_t dvp_transfer_s::buf_gpu_stride_align_ = 1;
uint32_t dvp_transfer_s::sem_addr_align_       = 1;
uint32_t dvp_transfer_s::sem_alloc_size_       = sizeof(uint32_t);
uint32_t dvp_transfer_s::sem_payload_offset_   = 0;
uint32_t dvp_transfer_s::sem_payload_size_     = sizeof(uint32_t);

// ─── semaphore_s ─────────────────────────────────────────────────────────────

void dvp_transfer_s::semaphore_s::init(uint32_t alloc_size, uint32_t addr_align)
{
    const auto allocation_size = static_cast<size_t>(alloc_size) + addr_align - 1;
    mem_unaligned              = static_cast<uint32_t*>(std::malloc(allocation_size));
    if (mem_unaligned == nullptr) {
        throw std::bad_alloc();
    }

    void*  aligned = mem_unaligned;
    size_t space   = allocation_size;
    if (std::align(addr_align, alloc_size, aligned, space) == nullptr) {
        std::free(mem_unaligned);
        mem_unaligned = nullptr;
        throw std::runtime_error("Unable to align DVP semaphore memory");
    }
    mem = static_cast<uint32_t*>(aligned);

    mem[0]        = 0;
    release_value = 0;
    acquire_value = 0;

    DVPSyncObjectDesc desc{};
    desc.externalClientWaitFunc = nullptr;
    desc.sem                    = mem;
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
        std::free(mem_unaligned);
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
        DVP_CHECK(dvpCloseGLContext());
    }

    dvp_supported_   = false;
    ctx_initialized_ = false;
}

bool dvp_transfer_s::register_texture_impl(texture_s* texture)
{
    if (texture_handle_ != 0) {
        return false;
    }
    DVP_CHECK(dvpCreateGPUTextureGL(texture->id(), &texture_handle_));

    // Initialise ownership to "API/GL has the texture" so the first
    // dvpMapBufferWaitDVP inside transfer() works correctly.
    dvpMapBufferEndAPI(texture_handle_);

    return true;
}

bool dvp_transfer_s::unregister_texture_impl(texture_s* /*texture*/)
{
    if (texture_handle_ == 0) {
        return false;
    }
    DVP_CHECK(dvpFreeBuffer(texture_handle_));
    texture_handle_ = 0;
    return true;
}

bool dvp_transfer_s::begin_texture_use_impl(texture_s* /*texture*/)
{
    if (texture_handle_ == 0) {
        return false;
    }
    // Tell GL it can use the texture: waits for DVP to have signalled EndDVP.
    dvpMapBufferWaitAPI(texture_handle_);
    return true;
}

bool dvp_transfer_s::end_texture_use_impl(texture_s* /*texture*/)
{
    if (texture_handle_ == 0) {
        return false;
    }
    // Signal to DVP that GL is done with the texture.
    dvpMapBufferEndAPI(texture_handle_);
    return true;
}

// ─── Constructor / Destructor ────────────────────────────────────────────────

dvp_transfer_s::dvp_transfer_s(size_t size, direction_e dir)
    : backend_i(size, dir)
{
    assert(dvp_supported_);

    // Allocate page-aligned, page-locked system memory.
    // buf_addr_align_ is the DVP requirement; use at least 4096 (page size).
    const size_t alignment = std::max(static_cast<size_t>(buf_addr_align_), static_cast<size_t>(4096));
    data_                  = aligned_alloc(alignment, size_);

    if (data_ == nullptr) {
        throw std::runtime_error("DVP: failed to allocate aligned sysmem");
    }

#ifdef _WIN32
    if (!VirtualLock(data_, size_)) {
        free(data_);
        data_ = nullptr;
        throw std::runtime_error("DVP: VirtualLock failed");
    }
#else
    if (mlock(data_, size_) != 0) {
        free(data_);
        data_ = nullptr;
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
    desc.bufAddr = data_;

    DVP_CHECK(dvpCreateBuffer(&desc, &sysmem_handle_));
    DVP_CHECK(dvpBindToGLCtx(sysmem_handle_));

    // Initialise semaphores.
    ext_sync_.init(sem_alloc_size_, sem_addr_align_);
    gpu_sync_.init(sem_alloc_size_, sem_addr_align_);
}

dvp_transfer_s::~dvp_transfer_s()
{
    if (texture_handle_ != 0) {
        DVP_CHECK(dvpFreeBuffer(texture_handle_));
        texture_handle_ = 0;
    }
    if (sysmem_handle_ != 0) {
        DVP_CHECK(dvpUnbindFromGLCtx(sysmem_handle_));
        DVP_CHECK(dvpDestroyBuffer(sysmem_handle_));
        sysmem_handle_ = 0;
    }

    ext_sync_.destroy();
    gpu_sync_.destroy();

    if (data_ != nullptr) {
#ifdef _WIN32
        VirtualUnlock(data_, size_);
#else
        munlock(data_, size_);
#endif
        free(data_);
        data_ = nullptr;
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

// ─── backend_i interface ─────────────────────────────────────────────────────

bool dvp_transfer_s::transfer()
{
    if (sysmem_handle_ == 0 || texture_handle_ == 0) {
        return false;
    }

    const auto dims = texture()->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        // CPU wrote data into data_; DMA data_ → texture.
        // endTextureInUse (dvpMapBufferEndAPI) must have been called by the caller
        // before this point so DVP knows GL is done with the texture.
        perform_dma(sysmem_handle_, texture_handle_, static_cast<uint32_t>(dims.y));
    } else {
        // GPU rendered to texture; DMA texture → data_.
        // Signal that GL is done writing to the texture, DVP can start reading.
        dvpMapBufferEndAPI(texture_handle_);
        perform_dma(texture_handle_, sysmem_handle_, static_cast<uint32_t>(dims.y));
    }

    return true;
}

bool dvp_transfer_s::wait_for_completion()
{
    if (direction_ == direction_e::cpu_to_gpu) {
        // For cpu_to_gpu: insert a GL wait so the GPU won't sample the texture
        // before DVP finishes writing it.  Must be called with GL context current.
        if (texture_handle_ != 0) {
            dvpMapBufferWaitAPI(texture_handle_);
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
