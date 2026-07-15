#include "persistent.hpp"

#include "gpu/texture.hpp"

#include <chrono>
#include <cstring>

namespace miximus::gpu::transfer::detail {

pinned_transfer_s::pinned_transfer_s(size_t size, direction_e dir)
    : transfer_i(size, dir)
{
    if (direction_ == direction_e::cpu_to_gpu) {
        // Separate CPU allocation for the external writer (e.g. DeckLink) to write into.
        // The PBO is created lazily on the first perform_copy() when a GL context is available.
        allocate_ptr();
    }
    // gpu_to_cpu: ptr_ will be set to the PBO mapped address in perform_transfer().
    // No separate allocation — the PBO IS the destination buffer.
}

pinned_transfer_s::~pinned_transfer_s()
{
    if (mapped_ptr_ != nullptr) {
        glUnmapNamedBuffer(id_);
        glDeleteBuffers(1, &id_);
    }

    if (direction_ == direction_e::cpu_to_gpu) {
        // Only the cpu_to_gpu path has a separately-allocated ptr_.
        free_ptr();
    }
    // gpu_to_cpu: ptr_ == mapped_ptr_ — freed by glDeleteBuffers above.
}

bool pinned_transfer_s::perform_copy()
{
    if (direction_ == direction_e::cpu_to_gpu) {
        // Create the write-mapped PBO on first call (lazy, requires GL context).
        if (mapped_ptr_ == nullptr) {
            const GLbitfield storage_flags =
                static_cast<GLbitfield>(GL_MAP_PERSISTENT_BIT) | static_cast<GLbitfield>(GL_MAP_WRITE_BIT);
            const GLbitfield map_flags = storage_flags | static_cast<GLbitfield>(GL_MAP_FLUSH_EXPLICIT_BIT);

            glCreateBuffers(1, &id_);
            glNamedBufferStorage(id_, static_cast<GLsizeiptr>(size_), nullptr, storage_flags);
            mapped_ptr_ = glMapNamedBufferRange(id_, 0, static_cast<GLsizeiptr>(size_), map_flags);
        }

        std::memcpy(mapped_ptr_, ptr_, size_);
        glFlushMappedNamedBufferRange(id_, 0, static_cast<GLsizeiptr>(size_));
        sync_ = std::make_unique<sync_s>();
    }
    // gpu_to_cpu: no-op — ptr_ already points to the PBO mapped memory.
    return true;
}

bool pinned_transfer_s::perform_transfer(texture_s* texture)
{
    const auto id   = texture->id();
    const auto dims = texture->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id_);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dims.x, dims.y, texture->format(), texture->type(), nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else {
        ensure_read_pbo(static_cast<GLsizeiptr>(size_));

        glBindBuffer(GL_PIXEL_PACK_BUFFER, id_);
        glBindTexture(GL_TEXTURE_2D, id);
        glGetTexImage(GL_TEXTURE_2D, 0, texture->format(), texture->type(), nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
        sync_ = std::make_unique<sync_s>();
    }

    return true;
}

bool pinned_transfer_s::wait_for_copy()
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

void pinned_transfer_s::ensure_read_pbo(GLsizeiptr size)
{
    if (mapped_ptr_ != nullptr) {
        return;
    }

    const GLbitfield flags = static_cast<GLbitfield>(GL_MAP_PERSISTENT_BIT) | static_cast<GLbitfield>(GL_MAP_READ_BIT);

    glCreateBuffers(1, &id_);
    glNamedBufferStorage(id_, size, nullptr, flags);
    mapped_ptr_ = glMapNamedBufferRange(id_, 0, size, flags);

    // ptr_ IS the PBO mapped memory — no separate allocation for gpu_to_cpu.
    ptr_ = mapped_ptr_;
}

} // namespace miximus::gpu::transfer::detail
