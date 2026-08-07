#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace miximus::core {

struct origin_info_s
{
    int64_t                    id{};
    std::optional<std::string> token;
};

} // namespace miximus::core
