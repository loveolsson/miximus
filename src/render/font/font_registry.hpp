#pragma once
#include "font_info.hpp"

#include <string_view>
#include <vector>

namespace miximus::render {

class font_registry_s
{
    std::map<std::string, font_info_s, std::less<>> fonts_;

    void log_fonts();
    void scan_fonts();

  public:
    font_registry_s();
    ~font_registry_s() = default;

    void refresh();

    const font_info_s*            find_font(std::string_view name) const;
    const font_variant_s*         find_font_variant(std::string_view name, std::string_view variant) const;
    std::vector<std::string_view> get_font_names() const;
    std::vector<std::string_view> get_font_variant_names(std::string_view name) const;

    static std::unique_ptr<font_registry_s> create_font_registry();
};

} // namespace miximus::render
