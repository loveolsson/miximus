#include "core/app_state.hpp"

namespace miximus::core {

app_state_s::app_state_s()
{
    gpu_ctx_      = std::make_unique<gpu::context>();
    shader_store_ = std::make_unique<gpu::shader_store_s>();
}

app_state_s::~app_state_s()
{
    shader_store_.reset();
    gpu_ctx_.reset();
    gpu::context::terminate();
}

} // namespace miximus::core
