#include "persistent.hpp"
#include "gpu/texture.hpp"
#include <cstring>

namespace miximus::gpu::transfer::detail {

pinned_transfer_s::pinned_transfer_s(size_t size, direction_e dir)
    : transfer_i(size, dir)
{
    allocate_ptr();
}

pinned_transfer_s::~pinned_transfer_s()
{
    if (mapped_ptr_ != nullptr) {
        GLenum target{};

        if (direction_ == direction_e::cpu_to_gpu) {
            target = GL_PIXEL_UNPACK_BUFFER;
        } else {
            target = GL_PIXEL_PACK_BUFFER;
        }

        glBindBuffer(target, id_);
        glUnmapBuffer(target);
        glBindBuffer(target, 0);
        glDeleteBuffers(1, &id_);
    }

    free_ptr();
}

bool pinned_transfer_s::perform_copy()
{
    if (direction_ == direction_e::cpu_to_gpu) {
        if (mapped_ptr_ == nullptr) {
            GLbitfield flags = GLbitfield(GL_MAP_PERSISTENT_BIT) | GLbitfield(GL_MAP_WRITE_BIT);

            glCreateBuffers(1, &id_);
            glNamedBufferStorage(id_, size_, nullptr, flags);

            flags |= GLbitfield(GL_MAP_FLUSH_EXPLICIT_BIT);
            mapped_ptr_ = glMapNamedBufferRange(id_, 0, size_, flags);

            assert(mapped_ptr_ != nullptr);
        }

        auto* c = reinterpret_cast<char*>(ptr_);
        std::copy(c, c + size_, reinterpret_cast<char*>(mapped_ptr_));
        glFlushMappedNamedBufferRange(id_, 0, size_);

        sync_ = std::make_unique<sync_s>();
    } else {
        if (mapped_ptr_ != nullptr) {
            auto* c = reinterpret_cast<char*>(mapped_ptr_);
            std::copy(c, c + size_, reinterpret_cast<char*>(ptr_));
        }
    }

    return true;
}

bool pinned_transfer_s::perform_transfer(texture_s* texture)
{
    auto id   = texture->id();
    auto dims = texture->texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id_);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dims.x, dims.y, texture->format(), texture->type(), nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else {
        if (mapped_ptr_ == nullptr) {
            GLbitfield flags = GLbitfield(GL_MAP_PERSISTENT_BIT) | GLbitfield(GL_MAP_READ_BIT);

            glCreateBuffers(1, &id_);
            glNamedBufferStorage(id_, size_, nullptr, flags);

            mapped_ptr_ = glMapNamedBufferRange(id_, 0, size_, flags);

            assert(mapped_ptr_ != nullptr);
        }

        if (id_ != 0) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, id_);
            glBindTexture(GL_TEXTURE_2D, id);
            glGetTexImage(GL_TEXTURE_2D, 0, texture->format(), texture->type(), nullptr);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
            sync_ = std::make_unique<sync_s>();
        }
    }

    return true;
}

bool pinned_transfer_s::wait_for_copy()
{
    if (sync_) {
        if (direction_ == direction_e::cpu_to_gpu) {
            sync_->gpu_wait();
        } else {
            sync_->cpu_wait(std::chrono::milliseconds(100));
        }

        sync_.reset();
        return true;
    }

    return false;
}

} // namespace miximus::gpu::transfer::detail