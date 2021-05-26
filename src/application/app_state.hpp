#pragma once
#include "gpu/context.hpp"
#include "gpu/shader.hpp"

namespace miximus::application {

class state
{
    gpu::context      gpu_ctx;
    gpu::shader_store shader_store;
};

} // namespace miximus::application
