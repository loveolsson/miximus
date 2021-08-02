#pragma once
#include "gpu/transfer/transfer.hpp"

namespace miximus::gpu::transfer::detail {

class fallback_transfer_s : public transfer_i
{
  public:
    fallback_transfer_s(size_t size, direction_e dir);
    ~fallback_transfer_s() = default;

    bool perform_copy() final { return true; }
    bool perform_transfer(texture_s*) final;
    bool wait_for_copy() final { return true; }
};

} // namespace miximus::gpu::transfer::detail
