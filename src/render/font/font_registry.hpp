#pragma once
#include "font_info.hpp"

#include <string_view>
#include <vector>

namespace miximus::render {

class font_registry_s
{
    std::map<std::string, font_info_s> fonts_;

    void log_fonts();

  public:
    font_registry_s();
    ~font_registry_s() = default;

    const font_info_s*            find_font(const std::string& name) const;
    const font_variant_s*         find_font_variant(const std::string& name, const std::string& variant) const;
    std::vector<std::string_view> get_font_names() const;
    std::vector<std::string_view> get_font_variant_names(const std::string& name) const;

    static std::unique_ptr<font_registry_s> create_font_registry();
};

} // namespace miximus::render
