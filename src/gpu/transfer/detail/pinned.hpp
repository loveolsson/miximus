#pragma once
#include "gpu/glad.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/transfer.hpp"

namespace miximus::gpu::transfer::detail {

class pinned_transfer_s : public transfer_i
{
    void*                   mapped_ptr_{};
    GLuint                  id_{};
    std::unique_ptr<sync_s> sync_;

  public:
    pinned_transfer_s(size_t size, direction_e dir);
    ~pinned_transfer_s();

    bool perform_copy() final;
    bool perform_transfer(texture_s*) final;
    bool wait_for_transfer() final;
};

} // namespace miximus::gpu::transfer::detail
