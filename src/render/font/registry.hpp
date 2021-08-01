#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace miximus::render::font {

struct font_variant_s
{
    std::string           name;
    std::filesystem::path path;
};

struct font_s
{
    std::string                           name;
    std::map<std::string, font_variant_s> variants;
};

class font_registry_s
{
    std::map<std::string, font_s> fonts_;

  public:
    font_registry_s();
    ~font_registry_s();

    const font_s*         find_font(const std::string& name) const;
    const font_variant_s* find_font_variant(const std::string& name, const std::string& variant) const;

    std::vector<std::string_view> get_font_names() const;
    std::vector<std::string_view> get_font_variants(const std::string& name) const;
};

} // namespace miximus::render::font
