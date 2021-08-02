#include "registry.hpp"
#include "logger/logger.hpp"

namespace miximus::render::font {

font_registry_s::~font_registry_s() {}

void font_registry_s::log_fonts()
{
    auto log = getlog("app");
    log->debug("Found system fonts:");
    for (const auto& [name, font] : fonts_) {
        log->debug("  \"{}\"", name);

        for (const auto& [v_name, variant] : font.variants) {
            log->debug("   -- {}: \"{}\", {}", v_name, variant.path.u8string(), variant.index);
        }
    }
}

const font_info_s* font_registry_s::find_font(const std::string& name) const
{
    auto it = fonts_.find(name);
    if (it != fonts_.end()) {
        return &it->second;
    }

    return nullptr;
}

const font_variant_s* font_registry_s::find_font_variant(const std::string& name, const std::string& variant) const
{
    const auto* font = find_font(name);
    if (font == nullptr) {
        return nullptr;
    }

    auto it = font->variants.find(variant);
    if (it != font->variants.end()) {
        return &it->second;
    }

    return nullptr;
}

std::vector<std::string_view> font_registry_s::get_font_names() const
{
    std::vector<std::string_view> res;
    res.reserve(fonts_.size());

    for (auto& [name, _] : fonts_) {
        res.emplace_back(name);
    }

    return res;
}

std::vector<std::string_view> font_registry_s::get_font_variant_names(const std::string& name) const
{
    std::vector<std::string_view> res;

    if (const auto* font = find_font(name)) {
        res.reserve(font->variants.size());

        for (const auto& [name, _] : font->variants) {
            res.emplace_back(name);
        }
    }

    return res;
}

} // namespace miximus::render::font