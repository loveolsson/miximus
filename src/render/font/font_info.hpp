#pragma once
#include "utils/string_map.hpp"

#include <filesystem>
#include <string>

namespace miximus::render {

struct font_variant_s
{
    int                   index;
    std::string           name;
    std::filesystem::path path;
};

struct font_info_s
{
    std::string                         name;
    utils::string_map_t<font_variant_s> variants;
};

} // namespace miximus::render
