#pragma once
#include "gpu/fence.hpp"
#include "gpu/glad.hpp"
#include "gpu/transfer/texture_transfer.hpp"
#include "texture_transfer_backend.hpp"

namespace miximus::gpu::transfer::detail {

class pinned_transfer_s : public texture_transfer_backend_i
{
    void*                    mapped_ptr_{};
    GLuint                   id_{};
    GLint                    row_length_{};
    std::unique_ptr<fence_s> transfer_fence_;

  public:
    pinned_transfer_s(const texture_transfer_layout_s& transfer_layout, direction_e dir);
    ~pinned_transfer_s();

    bool submit_transfer() final;
    bool wait_for_transfer_completion() final;
};

} // namespace miximus::gpu::transfer::detail
