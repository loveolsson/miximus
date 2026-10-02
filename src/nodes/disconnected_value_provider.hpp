#pragma once

#include "core/app_state_fwd.hpp"
#include "nodes/source_info.hpp"

#include <memory>

namespace miximus::nodes::detail {

template <typename T>
struct disconnected_value_provider_s
{
    void release() noexcept {}
    T    get(core::app_state_s* /*app*/, const T& fallback) { return fallback; }
};

template <>
struct disconnected_value_provider_s<framebuffer_source_info_s>
{
  private:
    std::unique_ptr<gpu::texture_s> framebuffer_;

  public:
    disconnected_value_provider_s();
    ~disconnected_value_provider_s();

    disconnected_value_provider_s(const disconnected_value_provider_s&)            = delete;
    disconnected_value_provider_s(disconnected_value_provider_s&&)                 = delete;
    disconnected_value_provider_s& operator=(const disconnected_value_provider_s&) = delete;
    disconnected_value_provider_s& operator=(disconnected_value_provider_s&&)      = delete;

    void release() noexcept;
    framebuffer_source_info_s
    get(core::app_state_s* app, const framebuffer_source_info_s& fallback, std::string_view name);
};

} // namespace miximus::nodes::detail
