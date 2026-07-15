#pragma once
#include "backend.hpp"
#include "gpu/glad.hpp"
#include "gpu/sync.hpp"

namespace miximus::gpu::transfer::detail {

class pinned_transfer_s : public backend_i
{
    void*                   mapped_ptr_{};
    GLuint                  id_{};
    std::unique_ptr<sync_s> sync_;

  public:
    pinned_transfer_s(size_t size, direction_e dir);
    ~pinned_transfer_s();

    bool transfer() final;
    bool wait_for_completion() final;

  private:
    void ensure_read_pbo(GLsizeiptr size);
};

} // namespace miximus::gpu::transfer::detail
