#pragma once
#include "backend.hpp"
#include "gpu/glad.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/texture_transfer.hpp"

namespace miximus::gpu::transfer::detail {

class pinned_transfer_s : public backend_i
{
    void*                   mapped_ptr_{};
    GLuint                  id_{};
    GLint                   row_length_{};
    std::unique_ptr<sync_s> sync_;

  public:
    pinned_transfer_s(const texture_transfer_requirements_s& requirements, direction_e dir);
    ~pinned_transfer_s();

    bool transfer() final;
    bool wait_for_completion() final;
};

} // namespace miximus::gpu::transfer::detail
