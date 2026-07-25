#pragma once

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace miximus::utils {

inline std::filesystem::path path_from_utf8(std::string_view value)
{
    std::u8string utf8(value.size(), u8'\0');
    std::memcpy(utf8.data(), value.data(), value.size());
    return std::filesystem::path(utf8);
}

inline std::string path_to_utf8(const std::filesystem::path& value)
{
    const auto  utf8 = value.u8string();
    std::string result(utf8.size(), '\0');
    std::memcpy(result.data(), utf8.data(), utf8.size());
    return result;
}

inline std::string path_to_generic_utf8(const std::filesystem::path& value)
{
    const auto  utf8 = value.generic_u8string();
    std::string result(utf8.size(), '\0');
    std::memcpy(result.data(), utf8.data(), utf8.size());
    return result;
}

} // namespace miximus::utils
