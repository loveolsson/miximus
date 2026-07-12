#include "cuda.hpp"

#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "logger/logger.hpp"

#include <array>
#include <stdexcept>

namespace miximus::gpu::transfer::detail {

namespace {
auto log() { return getlog("gpu"); }

bool check_cuda(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }

    log()->error("CUDA call failed: {}: {}", operation, cudaGetErrorString(status));
    return false;
}
} // namespace

bool cuda_transfer_s::initialized_ = false;
bool cuda_transfer_s::supported_   = false;
int  cuda_transfer_s::device_      = 0;

bool cuda_transfer_s::initialize_context()
{
    if (initialized_) {
        return supported_;
    }
    initialized_ = true;

    std::array<int, 8> devices{};
    unsigned int       device_count = 0;
    const auto         status =
        cudaGLGetDevices(&device_count, devices.data(), static_cast<unsigned int>(devices.size()), cudaGLDeviceListAll);
    if (status != cudaSuccess) {
        log()->info("Transfer: CUDA/OpenGL interop unavailable: {}", cudaGetErrorString(status));
        return false;
    }
    if (device_count == 0) {
        log()->info("Transfer: CUDA/OpenGL interop unavailable: no CUDA device owns the current OpenGL context");
        return false;
    }

    device_ = devices.front();
    if (!check_cuda(cudaSetDevice(device_), "cudaSetDevice")) {
        return false;
    }

    cudaDeviceProp properties{};
    if (!check_cuda(cudaGetDeviceProperties(&properties, device_), "cudaGetDeviceProperties")) {
        return false;
    }

    supported_ = true;
    log()->info("CUDA/OpenGL interop initialised on device {} ({})", device_, properties.name);
    return true;
}

void cuda_transfer_s::shutdown_context()
{
    if (!initialized_) {
        return;
    }

    if (supported_) {
        (void)check_cuda(cudaSetDevice(device_), "cudaSetDevice during shutdown");
        (void)check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize during shutdown");
    }

    supported_   = false;
    initialized_ = false;
    device_      = 0;
}

cuda_transfer_s::cuda_transfer_s(size_t size, direction_e dir)
    : transfer_i(size, dir)
{
    if (!supported_ || !check_cuda(cudaSetDevice(device_), "cudaSetDevice during transfer creation")) {
        throw std::runtime_error("CUDA transfer created without an initialized CUDA/OpenGL context");
    }
    if (!check_cuda(cudaHostAlloc(&ptr_, size_, cudaHostAllocPortable), "cudaHostAlloc")) {
        throw std::runtime_error("Failed to create CUDA transfer resources");
    }
    if (!check_cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreateWithFlags")) {
        (void)cudaFreeHost(ptr_);
        ptr_ = nullptr;
        throw std::runtime_error("Failed to create CUDA transfer resources");
    }
    if (!check_cuda(cudaEventCreateWithFlags(&completion_, cudaEventDisableTiming), "cudaEventCreateWithFlags")) {
        (void)cudaStreamDestroy(stream_);
        (void)cudaFreeHost(ptr_);
        stream_ = nullptr;
        ptr_    = nullptr;
        throw std::runtime_error("Failed to create CUDA transfer resources");
    }
}

cuda_transfer_s::~cuda_transfer_s()
{
    (void)cudaSetDevice(device_);
    if (stream_ != nullptr) {
        (void)cudaStreamSynchronize(stream_);
    }
    if (completion_ != nullptr) {
        (void)cudaEventDestroy(completion_);
    }
    if (buffer_resource_ != nullptr) {
        (void)cudaGraphicsUnregisterResource(buffer_resource_);
    }
    if (buffer_ != 0) {
        glDeleteBuffers(1, &buffer_);
    }
    if (stream_ != nullptr) {
        (void)cudaStreamDestroy(stream_);
    }
    if (ptr_ != nullptr) {
        (void)cudaFreeHost(ptr_);
    }
}

bool cuda_transfer_s::ensure_buffer()
{
    if (buffer_resource_ != nullptr) {
        return true;
    }
    if (!check_cuda(cudaSetDevice(device_), "cudaSetDevice during transfer")) {
        return false;
    }

    glCreateBuffers(1, &buffer_);
    glNamedBufferStorage(buffer_, static_cast<GLsizeiptr>(size_), nullptr, GL_DYNAMIC_STORAGE_BIT);

    const unsigned int flags =
        direction_ == direction_e::cpu_to_gpu ? cudaGraphicsRegisterFlagsWriteDiscard : cudaGraphicsRegisterFlagsNone;
    if (!check_cuda(cudaGraphicsGLRegisterBuffer(&buffer_resource_, buffer_, flags), "cudaGraphicsGLRegisterBuffer")) {
        glDeleteBuffers(1, &buffer_);
        buffer_ = 0;
        return false;
    }
    return true;
}

bool cuda_transfer_s::copy_host_to_buffer()
{
    if (!ensure_buffer() ||
        !check_cuda(cudaGraphicsMapResources(1, &buffer_resource_, stream_), "cudaGraphicsMapResources")) {
        return false;
    }

    void*  buffer_ptr  = nullptr;
    size_t buffer_size = 0;
    bool   success     = check_cuda(cudaGraphicsResourceGetMappedPointer(&buffer_ptr, &buffer_size, buffer_resource_),
                              "cudaGraphicsResourceGetMappedPointer");
    if (success && buffer_size < size_) {
        log()->error("CUDA: mapped buffer is too small ({} < {})", buffer_size, size_);
        success = false;
    }
    if (success) {
        success = check_cuda(cudaMemcpyAsync(buffer_ptr, ptr_, size_, cudaMemcpyHostToDevice, stream_),
                             "cudaMemcpyAsync host to buffer");
    }

    success =
        check_cuda(cudaGraphicsUnmapResources(1, &buffer_resource_, stream_), "cudaGraphicsUnmapResources") && success;
    success  = check_cuda(cudaEventRecord(completion_, stream_), "cudaEventRecord") && success;
    pending_ = success;
    return success;
}

bool cuda_transfer_s::copy_buffer_to_host()
{
    if (!ensure_buffer() ||
        !check_cuda(cudaGraphicsMapResources(1, &buffer_resource_, stream_), "cudaGraphicsMapResources")) {
        return false;
    }

    void*  buffer_ptr  = nullptr;
    size_t buffer_size = 0;
    bool   success     = check_cuda(cudaGraphicsResourceGetMappedPointer(&buffer_ptr, &buffer_size, buffer_resource_),
                              "cudaGraphicsResourceGetMappedPointer");
    if (success && buffer_size < size_) {
        log()->error("CUDA: mapped buffer is too small ({} < {})", buffer_size, size_);
        success = false;
    }
    if (success) {
        success = check_cuda(cudaMemcpyAsync(ptr_, buffer_ptr, size_, cudaMemcpyDeviceToHost, stream_),
                             "cudaMemcpyAsync buffer to host");
    }

    success =
        check_cuda(cudaGraphicsUnmapResources(1, &buffer_resource_, stream_), "cudaGraphicsUnmapResources") && success;
    success  = check_cuda(cudaEventRecord(completion_, stream_), "cudaEventRecord") && success;
    pending_ = success;
    return success;
}

bool cuda_transfer_s::perform_transfer(texture_s* texture)
{
    const auto dimensions = texture->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        if (!copy_host_to_buffer()) {
            return false;
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer_);
        glTextureSubImage2D(
            texture->id(), 0, 0, 0, dimensions.x, dimensions.y, texture->format(), texture->type(), nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return true;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_);
    glBindTexture(GL_TEXTURE_2D, texture->id());
    glGetTexImage(GL_TEXTURE_2D, 0, texture->format(), texture->type(), nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return copy_buffer_to_host();
}

bool cuda_transfer_s::perform_transfer(framebuffer_s* framebuffer)
{
    if (direction_ != direction_e::gpu_to_cpu || !ensure_buffer()) {
        return false;
    }

    const auto dimensions = framebuffer->texture()->texture_dimensions();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_);
    glReadPixels(
        0, 0, dimensions.x, dimensions.y, framebuffer->texture()->format(), framebuffer->texture()->type(), nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return copy_buffer_to_host();
}

bool cuda_transfer_s::wait_for_copy()
{
    if (!pending_) {
        return false;
    }

    if (!check_cuda(cudaSetDevice(device_), "cudaSetDevice while waiting for transfer")) {
        return false;
    }

    const bool success = check_cuda(cudaEventSynchronize(completion_), "cudaEventSynchronize");
    pending_           = false;
    return success;
}

} // namespace miximus::gpu::transfer::detail
