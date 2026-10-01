#include "external_image_support.hpp"

#include <array>

namespace miximus::gpu::detail {
namespace {

constexpr std::array<std::string_view, 1> REQUIRED_EXTENSIONS{
    "VK_KHR_external_memory_win32",
};

} // namespace

std::span<const std::string_view> external_image_import_extensions() { return REQUIRED_EXTENSIONS; }

} // namespace miximus::gpu::detail
