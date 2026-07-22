#include "cuda.hpp"

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

bool cuda_transfer_s::supports_direct_image(texture_s::format_e format)
{
    // Direct copies require both identical host/storage bytes and an OpenGL
    // internal format supported by cudaGraphicsGLRegisterImage. CUDA does not
    // support packed GL_RGB10_A2 images, even though uyuv_u10 has an identical
    // four-byte host and texture-storage representation.
    switch (format) {
        case texture_s::format_e::rgba_u8:
            return texture_s::format_info(format).storage_identical;
        case texture_s::format_e::rgb_f16:
        case texture_s::format_e::rgba_f16:
        case texture_s::format_e::bgra_u8:
        case texture_s::format_e::uyuv_u8:
        case texture_s::format_e::uyuv_u10:
            return false;
    }
    return false;
}

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

cuda_transfer_s::cuda_transfer_s(const texture_transfer_requirements_s& requirements,
                                 direction_e                            dir,
                                 bool                                   direct_image)
    : backend_i(requirements.byte_size, dir)
    , row_stride_(requirements.row_stride)
    , row_length_(static_cast<GLint>(requirements.row_stride /
                                     texture_s::format_info(requirements.format).host_bytes_per_texel))
    , direct_image_(direct_image)
{
    if (!supported_ || !check_cuda(cudaSetDevice(device_), "cudaSetDevice during transfer creation")) {
        throw std::runtime_error("CUDA transfer created without an initialized CUDA/OpenGL context");
    }
    auto host_flags = cudaHostAllocPortable;
    if (dir == direction_e::cpu_to_gpu && requirements.host_access == host_access_e::overwrite) {
        host_flags |= cudaHostAllocWriteCombined;
    }
    if (!check_cuda(cudaHostAlloc(&data_, size_, host_flags), "cudaHostAlloc")) {
        throw std::runtime_error("Failed to create CUDA transfer resources");
    }
    if (!check_cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreateWithFlags")) {
        (void)cudaFreeHost(data_);
        data_ = nullptr;
        throw std::runtime_error("Failed to create CUDA transfer resources");
    }
    if (!check_cuda(cudaEventCreateWithFlags(&completion_, cudaEventDisableTiming), "cudaEventCreateWithFlags")) {
        (void)cudaStreamDestroy(stream_);
        (void)cudaFreeHost(data_);
        stream_ = nullptr;
        data_   = nullptr;
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
    if (texture_resource_ != nullptr) {
        (void)cudaGraphicsUnregisterResource(texture_resource_);
    }
    if (buffer_ != 0) {
        glDeleteBuffers(1, &buffer_);
    }
    if (stream_ != nullptr) {
        (void)cudaStreamDestroy(stream_);
    }
    if (data_ != nullptr) {
        (void)cudaFreeHost(data_);
    }
}

bool cuda_transfer_s::ensure_texture_resource(texture_s* texture)
{
    if (texture_resource_ != nullptr && registered_texture_ == texture->id()) {
        return true;
    }
    if (texture_resource_ != nullptr) {
        if (!check_cuda(cudaGraphicsUnregisterResource(texture_resource_), "cudaGraphicsUnregisterResource image")) {
            return false;
        }
        texture_resource_   = nullptr;
        registered_texture_ = 0;
    }
    if (!check_cuda(cudaSetDevice(device_), "cudaSetDevice during image registration")) {
        return false;
    }
    const unsigned int flags = direction_ == direction_e::cpu_to_gpu ? cudaGraphicsRegisterFlagsWriteDiscard
                                                                     : cudaGraphicsRegisterFlagsReadOnly;
    if (!check_cuda(cudaGraphicsGLRegisterImage(&texture_resource_, texture->id(), GL_TEXTURE_2D, flags),
                    "cudaGraphicsGLRegisterImage")) {
        return false;
    }
    registered_texture_ = texture->id();
    return true;
}

bool cuda_transfer_s::register_texture_impl(texture_s* texture)
{
    return !direct_image_ || ensure_texture_resource(texture);
}

bool cuda_transfer_s::unregister_texture_impl(texture_s* /*texture*/)
{
    if (texture_resource_ == nullptr) {
        return true;
    }
    const bool success =
        check_cuda(cudaGraphicsUnregisterResource(texture_resource_), "cudaGraphicsUnregisterResource image");
    texture_resource_   = nullptr;
    registered_texture_ = 0;
    return success;
}

bool cuda_transfer_s::copy_host_to_texture(texture_s* texture)
{
    const auto dimensions = texture->texture_dimensions();
    const auto format     = texture_s::format_info(texture->color_type());
    const auto row_bytes  = static_cast<size_t>(dimensions.x) * format.storage_bytes_per_texel;
    if (row_stride_ < row_bytes || !ensure_texture_resource(texture) ||
        !check_cuda(cudaGraphicsMapResources(1, &texture_resource_, stream_), "cudaGraphicsMapResources image")) {
        return false;
    }

    cudaArray_t array{};
    bool        success = check_cuda(cudaGraphicsSubResourceGetMappedArray(&array, texture_resource_, 0, 0),
                              "cudaGraphicsSubResourceGetMappedArray");
    if (success) {
        success = check_cuda(cudaMemcpy2DToArrayAsync(array,
                                                      0,
                                                      0,
                                                      data_,
                                                      row_stride_,
                                                      row_bytes,
                                                      static_cast<size_t>(dimensions.y),
                                                      cudaMemcpyHostToDevice,
                                                      stream_),
                             "cudaMemcpy2DToArrayAsync host to texture");
    }

    success =
        check_cuda(cudaGraphicsUnmapResources(1, &texture_resource_, stream_), "cudaGraphicsUnmapResources image") &&
        success;
    success  = check_cuda(cudaEventRecord(completion_, stream_), "cudaEventRecord") && success;
    pending_ = success;
    return success;
}

bool cuda_transfer_s::copy_texture_to_host(texture_s* texture)
{
    const auto dimensions = texture->texture_dimensions();
    const auto format     = texture_s::format_info(texture->color_type());
    const auto row_bytes  = static_cast<size_t>(dimensions.x) * format.storage_bytes_per_texel;
    if (dimensions.y <= 0 || row_stride_ < row_bytes || !ensure_texture_resource(texture) ||
        !check_cuda(cudaGraphicsMapResources(1, &texture_resource_, stream_), "cudaGraphicsMapResources image")) {
        return false;
    }

    cudaArray_t array{};
    bool        success = check_cuda(cudaGraphicsSubResourceGetMappedArray(&array, texture_resource_, 0, 0),
                              "cudaGraphicsSubResourceGetMappedArray");
    if (success) {
        success = check_cuda(cudaMemcpy2DFromArrayAsync(data_,
                                                        row_stride_,
                                                        array,
                                                        0,
                                                        0,
                                                        row_bytes,
                                                        static_cast<size_t>(dimensions.y),
                                                        cudaMemcpyDeviceToHost,
                                                        stream_),
                             "cudaMemcpy2DFromArrayAsync texture to host");
    }

    success =
        check_cuda(cudaGraphicsUnmapResources(1, &texture_resource_, stream_), "cudaGraphicsUnmapResources image") &&
        success;
    success  = check_cuda(cudaEventRecord(completion_, stream_), "cudaEventRecord") && success;
    pending_ = success;
    return success;
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
        success = check_cuda(cudaMemcpyAsync(buffer_ptr, data_, size_, cudaMemcpyHostToDevice, stream_),
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
        success = check_cuda(cudaMemcpyAsync(data_, buffer_ptr, size_, cudaMemcpyDeviceToHost, stream_),
                             "cudaMemcpyAsync buffer to host");
    }

    success =
        check_cuda(cudaGraphicsUnmapResources(1, &buffer_resource_, stream_), "cudaGraphicsUnmapResources") && success;
    success  = check_cuda(cudaEventRecord(completion_, stream_), "cudaEventRecord") && success;
    pending_ = success;
    return success;
}

bool cuda_transfer_s::transfer()
{
    const auto dimensions = texture()->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        if (direct_image_) {
            return copy_host_to_texture(texture());
        }
        if (!copy_host_to_buffer()) {
            return false;
        }

        GLint previous_row_length{};
        GLint previous_alignment{};
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_row_length);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer_);
        glTextureSubImage2D(
            texture()->id(), 0, 0, 0, dimensions.x, dimensions.y, texture()->format(), texture()->type(), nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_row_length);
        glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
        return true;
    }

    if (direct_image_) {
        return copy_texture_to_host(texture());
    }

    if (!ensure_buffer()) {
        return false;
    }
    GLint previous_row_length{};
    GLint previous_alignment{};
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_row_length);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_);
    glBindTexture(GL_TEXTURE_2D, texture()->id());
    glGetTexImage(GL_TEXTURE_2D, 0, texture()->format(), texture()->type(), nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, previous_row_length);
    glPixelStorei(GL_PACK_ALIGNMENT, previous_alignment);
    return copy_buffer_to_host();
}

bool cuda_transfer_s::wait_for_completion()
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
