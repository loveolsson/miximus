#pragma once
#include "gpu/framebuffer_fwd.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/types.hpp"

#include <cmath>
#include <glm/common.hpp>
#include <glm/vector_relational.hpp>

namespace miximus::utils {

template <typename T>
bool is_finite(const T& value) = delete;

template <>
inline bool is_finite<double>(const double& value)
{
    return std::isfinite(value);
}

template <>
inline bool is_finite<gpu::vec2_t>(const gpu::vec2_t& value)
{
    return !glm::any(glm::isnan(value)) && !glm::any(glm::isinf(value));
}

template <>
inline bool is_finite<gpu::rect_s>(const gpu::rect_s& value)
{
    return is_finite(value.pos) && is_finite(value.size);
}

template <>
inline bool is_finite<gpu::texture_s*>(gpu::texture_s* const& /*value*/)
{
    return true;
}

template <>
inline bool is_finite<gpu::framebuffer_s*>(gpu::framebuffer_s* const& /*value*/)
{
    return true;
}

} // namespace miximus::utils
