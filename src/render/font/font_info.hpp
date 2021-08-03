#pragma once
#include <filesystem>
#include <map>
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
    std::string                           name;
    std::map<std::string, font_variant_s> variants;
};

} // namespace miximus::render