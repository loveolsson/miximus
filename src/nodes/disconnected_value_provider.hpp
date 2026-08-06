#pragma once

#include "core/app_state_fwd.hpp"
#include "gpu/framebuffer_fwd.hpp"

#include <memory>

namespace miximus::nodes::detail {

template <typename T>
struct disconnected_value_provider_s
{
    void release() noexcept {}
    T    get(core::app_state_s* /*app*/, const T& fallback) { return fallback; }
};

template <>
struct disconnected_value_provider_s<gpu::framebuffer_s*>
{
  private:
    std::unique_ptr<gpu::framebuffer_s> framebuffer_;

  public:
    disconnected_value_provider_s() = default;
    ~disconnected_value_provider_s();

    disconnected_value_provider_s(const disconnected_value_provider_s&)            = delete;
    disconnected_value_provider_s(disconnected_value_provider_s&&)                 = delete;
    disconnected_value_provider_s& operator=(const disconnected_value_provider_s&) = delete;
    disconnected_value_provider_s& operator=(disconnected_value_provider_s&&)      = delete;

    void                release() noexcept;
    gpu::framebuffer_s* get(core::app_state_s* app, gpu::framebuffer_s* const& fallback);
};

} // namespace miximus::nodes::detail
