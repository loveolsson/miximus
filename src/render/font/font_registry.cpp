#include "font_registry.hpp"

#include "logger/logger.hpp"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace miximus::render {

font_registry_s::font_registry_s() { refresh(); }

void font_registry_s::refresh()
{
    const std::unique_lock lock(font_mutex_);
    scan_fonts();
    ++font_list_version_;
}

void font_registry_s::log_fonts()
{
    auto log = getlog("app");
    log->debug("Found system fonts:");
    for (const auto& [name, font] : fonts_) {
        log->debug("  \"{}\"", name);

        for (const auto& [v_name, variant] : font.variants) {
            log->debug("   -- {}: \"{}\", {}", v_name, variant.path.string(), variant.index);
        }
    }
}

std::optional<font_info_s> font_registry_s::find_font(std::string_view name) const
{
    const std::shared_lock lock(font_mutex_);
    auto                   it = fonts_.find(name);
    if (it != fonts_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::optional<font_variant_s> font_registry_s::find_font_variant(std::string_view name, std::string_view variant) const
{
    const std::shared_lock lock(font_mutex_);
    const auto             font = fonts_.find(name);
    if (font == fonts_.end()) {
        return std::nullopt;
    }

    auto it = font->second.variants.find(variant);
    if (it != font->second.variants.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<std::string> font_registry_s::get_font_names() const
{
    const std::shared_lock   lock(font_mutex_);
    std::vector<std::string> res;
    res.reserve(fonts_.size());

    for (const auto& [name, _] : fonts_) {
        res.emplace_back(name);
    }

    return res;
}

std::vector<std::string> font_registry_s::get_font_variant_names(std::string_view name) const
{
    const std::shared_lock   lock(font_mutex_);
    std::vector<std::string> res;

    if (const auto font = fonts_.find(name); font != fonts_.end()) {
        res.reserve(font->second.variants.size());

        for (const auto& [variant, _] : font->second.variants) {
            res.emplace_back(variant);
        }
    }

    return res;
}

std::unique_ptr<font_registry_s> font_registry_s::create_font_registry() { return std::make_unique<font_registry_s>(); }

} // namespace miximus::render
