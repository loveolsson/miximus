#include "dvp.hpp"

#include "gpu/texture.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <malloc.h>
#else
#include <GL/glx.h>   // glXGetCurrentContext — detect EGL vs GLX before touching DVP
#include <sys/mman.h> // mlock / munlock
#endif

namespace miximus::gpu::transfer::detail {

namespace {
bool check_dvp_status(DVPStatus status, std::string_view command)
{
    if (status == DVP_STATUS_OK) {
        return true;
    }
    getlog("gpu")->error("DVP call failed: {} status={}", command, static_cast<int>(status));
    return false;
}

void require_dvp_status(DVPStatus status, std::string_view command)
{
    if (!check_dvp_status(status, command)) {
        throw std::runtime_error("DVP operation failed during transfer resource creation");
    }
}

struct dvp_format_s
{
    DVPBufferFormats format;
    DVPBufferTypes   type;
};

dvp_format_s get_dvp_format(texture_s::pixel_format_e pixel_format)
{
    switch (pixel_format) {
        case texture_s::pixel_format_e::rgb_f16:
            return {.format = DVP_RGB, .type = DVP_UNSIGNED_BYTE};
        case texture_s::pixel_format_e::rgba_f16:
        case texture_s::pixel_format_e::rgba_u8:
            return {.format = DVP_RGBA, .type = DVP_UNSIGNED_BYTE};
        case texture_s::pixel_format_e::argb_u8:
            return {.format = DVP_BGRA, .type = DVP_UNSIGNED_INT_8_8_8_8};
        case texture_s::pixel_format_e::bgra_u8:
        case texture_s::pixel_format_e::uyuv_u8:
            return {.format = DVP_BGRA, .type = DVP_UNSIGNED_INT_8_8_8_8_REV};
        case texture_s::pixel_format_e::uyuv_u10:
            return {.format = DVP_RGBA, .type = DVP_UNSIGNED_INT_2_10_10_10_REV};
    }
    throw std::invalid_argument("unsupported DVP texture format");
}

void* allocate_aligned(size_t alignment, size_t size)
{
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}

void free_aligned(void* ptr)
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
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

    DVPSyncObjectDesc config{
        .sem                    = mem,
        .flags                  = DVP_SYNC_OBJECT_FLAGS_USE_EVENTS,
        .externalClientWaitFunc = nullptr,
    };

    require_dvp_status(dvpImportSyncObject(&config, &dvp_handle), "dvpImportSyncObject");
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
    if (!check_dvp_status(dvpCreateGPUTextureGL(texture->id(), &texture_handle_), "dvpCreateGPUTextureGL")) {
        texture_handle_ = 0;
        return false;
    }

    // Initialise ownership to "API/GL has the texture" so the first
    // dvpMapBufferWaitDVP inside submit_transfer() works correctly.
    return check_dvp_status(dvpMapBufferEndAPI(texture_handle_), "dvpMapBufferEndAPI");
}

bool dvp_transfer_s::unregister_texture_impl(texture_s* /*texture*/)
{
    if (texture_handle_ == 0) {
        return false;
    }
    const bool success = check_dvp_status(dvpFreeBuffer(texture_handle_), "dvpFreeBuffer texture");
    texture_handle_    = 0;
    return success;
}

bool dvp_transfer_s::acquire_texture_for_gl_impl(texture_s* /*texture*/)
{
    if (texture_handle_ == 0) {
        return false;
    }
    // Tell GL it can use the texture: waits for DVP to have signalled EndDVP.
    return check_dvp_status(dvpMapBufferWaitAPI(texture_handle_), "dvpMapBufferWaitAPI");
}

bool dvp_transfer_s::release_texture_from_gl_impl(texture_s* /*texture*/)
{
    if (texture_handle_ == 0) {
        return false;
    }
    // Signal to DVP that GL is done with the texture.
    return check_dvp_status(dvpMapBufferEndAPI(texture_handle_), "dvpMapBufferEndAPI");
}

// ─── Constructor / Destructor ────────────────────────────────────────────────

dvp_transfer_s::dvp_transfer_s(const texture_transfer_layout_s& transfer_layout, direction_e dir)
    : texture_transfer_backend_i(transfer_layout.host_buffer_size_bytes, dir)
{
    assert(dvp_supported_);

    if (!supports(transfer_layout)) {
        throw std::invalid_argument("DVP transfer transfer_layout are unsupported");
    }

    // Allocate page-aligned, page-locked system memory.
    // buf_addr_align_ is the DVP requirement; use at least 4096 (page size).
    const size_t alignment =
        std::max({static_cast<size_t>(buf_addr_align_), transfer_layout.host_address_alignment_bytes, size_t{4096}});
    if (!std::has_single_bit(alignment) ||
        host_buffer_size_bytes_ > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        throw std::invalid_argument("DVP transfer alignment is invalid");
    }
    const size_t allocation_size = (host_buffer_size_bytes_ + alignment - 1) & ~(alignment - 1);
    host_memory_                 = allocate_aligned(alignment, allocation_size);

    if (host_memory_ == nullptr) {
        throw std::runtime_error("DVP: failed to allocate aligned sysmem");
    }

#ifdef _WIN32
    if (!VirtualLock(host_memory_, host_buffer_size_bytes_)) {
        free_aligned(host_memory_);
        host_memory_ = nullptr;
        throw std::runtime_error("DVP: VirtualLock failed");
    }
#else
    if (mlock(host_memory_, host_buffer_size_bytes_) != 0) {
        free_aligned(host_memory_);
        host_memory_ = nullptr;
        throw std::runtime_error("DVP: mlock failed — check RLIMIT_MEMLOCK");
    }
#endif

    // Register the sysmem buffer with DVP.
    const auto texture_dimensions = transfer_layout.dimensions.x /
                                    texture_s::pixel_format_info(transfer_layout.pixel_format).display_pixels_per_texel;
    const auto          dvp_format = get_dvp_format(transfer_layout.pixel_format);
    DVPSysmemBufferDesc config{
        .width   = static_cast<uint32_t>(texture_dimensions),
        .height  = static_cast<uint32_t>(transfer_layout.dimensions.y),
        .stride  = static_cast<uint32_t>(transfer_layout.host_row_stride_bytes),
        .size    = static_cast<uint32_t>(host_buffer_size_bytes_),
        .format  = dvp_format.format,
        .type    = dvp_format.type,
        .bufAddr = host_memory_,
    };

    bool bound_to_context = false;
    try {
        require_dvp_status(dvpCreateBuffer(&config, &sysmem_handle_), "dvpCreateBuffer");
        require_dvp_status(dvpBindToGLCtx(sysmem_handle_), "dvpBindToGLCtx");
        bound_to_context = true;

        ext_sync_.init(sem_alloc_size_, sem_addr_align_);
        gpu_sync_.init(sem_alloc_size_, sem_addr_align_);
    } catch (...) {
        if (sysmem_handle_ != 0) {
            if (bound_to_context) {
                (void)check_dvp_status(dvpUnbindFromGLCtx(sysmem_handle_), "dvpUnbindFromGLCtx after failed creation");
            }
            (void)check_dvp_status(dvpDestroyBuffer(sysmem_handle_), "dvpDestroyBuffer after failed creation");
            sysmem_handle_ = 0;
        }
#ifdef _WIN32
        VirtualUnlock(host_memory_, host_buffer_size_bytes_);
#else
        munlock(host_memory_, host_buffer_size_bytes_);
#endif
        free_aligned(host_memory_);
        host_memory_ = nullptr;
        throw;
    }

    if (buf_gpu_stride_align_ > 1 && transfer_layout.host_row_stride_bytes % buf_gpu_stride_align_ != 0) {
        getlog("gpu")->debug("DVP row stride {} does not meet the recommended {}-byte GPU alignment",
                             transfer_layout.host_row_stride_bytes,
                             buf_gpu_stride_align_);
    }
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

    if (host_memory_ != nullptr) {
#ifdef _WIN32
        VirtualUnlock(host_memory_, host_buffer_size_bytes_);
#else
        munlock(host_memory_, host_buffer_size_bytes_);
#endif
        free_aligned(host_memory_);
        host_memory_ = nullptr;
    }
}

// ─── Core DMA helper ─────────────────────────────────────────────────────────

bool dvp_transfer_s::perform_dma(DVPBufferHandle src, DVPBufferHandle dst, uint32_t height)
{
    gpu_sync_.release_value = gpu_sync_.release_value + 1;

    bool success = check_dvp_status(dvpBegin(), "dvpBegin");
    success      = check_dvp_status(dvpMapBufferWaitDVP(src), "dvpMapBufferWaitDVP") && success;
    success      = check_dvp_status(dvpMemcpyLined(src,
                                              ext_sync_.dvp_handle,
                                              ext_sync_.acquire_value,
                                              DVP_TIMEOUT_IGNORED,
                                              dst,
                                              gpu_sync_.dvp_handle,
                                              gpu_sync_.release_value,
                                              0,
                                              height),
                               "dvpMemcpyLined") &&
              success;
    success = check_dvp_status(dvpMapBufferEndDVP(src), "dvpMapBufferEndDVP") && success;
    return check_dvp_status(dvpEnd(), "dvpEnd") && success;
}

// ─── texture_transfer_backend_i interface ─────────────────────────────────────────────────────

bool dvp_transfer_s::submit_transfer()
{
    if (sysmem_handle_ == 0 || texture_handle_ == 0) {
        return false;
    }

    const auto dims = texture()->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        // CPU wrote data into host_memory_; DMA host_memory_ → texture.
        // endTextureInUse (dvpMapBufferEndAPI) must have been called by the caller
        // before this point so DVP knows GL is done with the texture.
        return perform_dma(sysmem_handle_, texture_handle_, static_cast<uint32_t>(dims.y));
    }

    // GPU rendered to texture; DMA texture → host_memory_.
    // Signal that GL is done writing to the texture, DVP can start reading.
    if (!check_dvp_status(dvpMapBufferEndAPI(texture_handle_), "dvpMapBufferEndAPI")) {
        return false;
    }
    return perform_dma(texture_handle_, sysmem_handle_, static_cast<uint32_t>(dims.y));
}

bool dvp_transfer_s::wait_for_transfer_completion()
{
    if (direction_ == direction_e::cpu_to_gpu) {
        // For cpu_to_gpu: insert a GL wait so the GPU won't sample the texture
        // before DVP finishes writing it.  Must be called with GL context current.
        if (texture_handle_ != 0) {
            return check_dvp_status(dvpMapBufferWaitAPI(texture_handle_), "dvpMapBufferWaitAPI");
        }
    } else {
        // For gpu_to_cpu: CPU blocks until DVP DMA into sysmem is complete.
        // No GL context required — dvpBegin/End and dvpSyncObjClientWaitComplete
        // are pure DVP / CPU operations.
        bool success = check_dvp_status(dvpBegin(), "dvpBegin");
        success      = check_dvp_status(dvpSyncObjClientWaitComplete(gpu_sync_.dvp_handle, DVP_TIMEOUT_IGNORED),
                                   "dvpSyncObjClientWaitComplete") &&
                  success;
        return check_dvp_status(dvpEnd(), "dvpEnd") && success;
    }
    return true;
}

bool dvp_transfer_s::supports(const texture_transfer_layout_s& transfer_layout)
{
    if (!dvp_supported_ || transfer_layout.dimensions.x <= 0 || transfer_layout.dimensions.y <= 0 ||
        transfer_layout.host_row_stride_bytes == 0 || transfer_layout.host_buffer_size_bytes == 0 ||
        transfer_layout.host_buffer_size_bytes > std::numeric_limits<uint32_t>::max() ||
        transfer_layout.host_row_stride_bytes > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    try {
        (void)get_dvp_format(transfer_layout.pixel_format);
        return transfer_layout.dimensions.x %
                   texture_s::pixel_format_info(transfer_layout.pixel_format).display_pixels_per_texel ==
               0;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

} // namespace miximus::gpu::transfer::detail
