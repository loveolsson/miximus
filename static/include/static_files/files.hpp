#pragma once
#include <unordered_map>
#include <string_view>

namespace miximus::static_files {
    extern const std::unordered_map<std::string_view, std::string_view> files;
}
