#pragma once
#include "gpu/context.hpp"
#include "gpu/shader.hpp"
#include <memory>

namespace miximus::core {

class app_state_s
{
    std::unique_ptr<gpu::context>        gpu_ctx_;
    std::unique_ptr<gpu::shader_store_s> shader_store_;

  public:
    app_state_s();
    ~app_state_s();
};

} // namespace miximus::core
