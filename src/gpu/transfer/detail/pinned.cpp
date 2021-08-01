#include "pinned.hpp"
#include "gpu/texture.hpp"
#include <cstring>

namespace miximus::gpu::transfer::detail {

pinned_transfer_s::pinned_transfer_s(size_t size, direction_e dir)
    : transfer_i(size, dir)
{
}

pinned_transfer_s::~pinned_transfer_s()
{
    if (mapped_ptr_ != nullptr) {
        GLenum target;

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
}

bool pinned_transfer_s::perform_copy()
{
    if (direction_ == direction_e::cpu_to_gpu) {
        if (mapped_ptr_ == nullptr) {
            GLbitfield flags = GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT;

            glCreateBuffers(1, &id_);
            glNamedBufferStorage(id_, size_, 0, flags);
            mapped_ptr_ = glMapNamedBufferRange(id_, 0, size_, flags);

            assert(mapped_ptr_ != nullptr);
        }

        auto c = reinterpret_cast<char*>(ptr_);
        std::copy(c, c + size_, reinterpret_cast<char*>(mapped_ptr_));
        glFlushMappedNamedBufferRange(id_, 0, size_);

        sync_ = std::make_unique<sync_s>();
    } else {
    }

    return true;
}

bool pinned_transfer_s::perform_transfer(texture_s& texture)
{
    auto id   = texture.id();
    auto dims = texture.texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id_);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dims.x, dims.y, texture.format(), texture.type(), 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return true;
    } else {
        return true;
    }
}

bool pinned_transfer_s::wait_for_transfer()
{
    if (sync_) {
        sync_->gpu_wait();
        sync_.reset();
        return true;
    }

    return false;
}

} // namespace miximus::gpu::transfer::detail