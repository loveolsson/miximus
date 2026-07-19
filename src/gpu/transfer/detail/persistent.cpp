#include "persistent.hpp"

#include "gpu/texture.hpp"

#include <chrono>
#include <stdexcept>

namespace miximus::gpu::transfer::detail {

pinned_transfer_s::pinned_transfer_s(const texture_transfer_requirements_s& requirements, direction_e dir)
    : backend_i(requirements.byte_size, dir)
    , row_length_(static_cast<GLint>(requirements.row_stride /
                                     texture_s::format_info(requirements.format).host_bytes_per_texel))
{
    GLbitfield storage_flags = GL_MAP_PERSISTENT_BIT;
    GLbitfield map_flags     = GL_MAP_PERSISTENT_BIT;
    if (direction_ == direction_e::cpu_to_gpu) {
        storage_flags |= GL_MAP_WRITE_BIT;
        map_flags |= GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
    } else {
        storage_flags |= GL_MAP_READ_BIT;
        map_flags |= GL_MAP_READ_BIT;
    }

    glCreateBuffers(1, &id_);
    glNamedBufferStorage(id_, static_cast<GLsizeiptr>(size_), nullptr, storage_flags);
    mapped_ptr_ = glMapNamedBufferRange(id_, 0, static_cast<GLsizeiptr>(size_), map_flags);
    if (mapped_ptr_ == nullptr) {
        glDeleteBuffers(1, &id_);
        id_ = 0;
        throw std::runtime_error("failed to map persistent pixel transfer buffer");
    }
    data_ = mapped_ptr_;
}

pinned_transfer_s::~pinned_transfer_s()
{
    if (mapped_ptr_ != nullptr) {
        glUnmapNamedBuffer(id_);
        mapped_ptr_ = nullptr;
        data_       = nullptr;
    }
    if (id_ != 0) {
        glDeleteBuffers(1, &id_);
        id_ = 0;
    }
}

bool pinned_transfer_s::transfer()
{
    const auto id   = texture()->id();
    const auto dims = texture()->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        glFlushMappedNamedBufferRange(id_, 0, static_cast<GLsizeiptr>(size_));
        GLint previous_row_length{};
        GLint previous_alignment{};
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_row_length);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id_);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dims.x, dims.y, texture()->format(), texture()->type(), nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_row_length);
        glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
        sync_ = std::make_unique<sync_s>();
    } else {
        GLint previous_row_length{};
        GLint previous_alignment{};
        glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_row_length);
        glGetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length_);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, id_);
        glBindTexture(GL_TEXTURE_2D, id);
        glGetTexImage(GL_TEXTURE_2D, 0, texture()->format(), texture()->type(), nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, previous_row_length);
        glPixelStorei(GL_PACK_ALIGNMENT, previous_alignment);

        glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
        sync_ = std::make_unique<sync_s>();
    }

    return true;
}

bool pinned_transfer_s::wait_for_completion()
{
    if (!sync_) {
        return false;
    }

    if (direction_ == direction_e::cpu_to_gpu) {
        sync_->gpu_wait();
    } else {
        // Indefinite wait — callers must ensure this is reached only after the
        // GPU work is done (e.g. via gpu::context_s::finish()) to avoid blocking.
        sync_->cpu_wait(std::chrono::hours(1));
    }

    sync_.reset();
    return true;
}

} // namespace miximus::gpu::transfer::detail
