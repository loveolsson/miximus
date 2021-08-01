#include "registry.hpp"
#include "logger/logger.hpp"

#include <fontconfig/fontconfig.h>

namespace miximus::render::font {

font_registry_s::font_registry_s()
{
    auto log = getlog("app");
    log->debug("Scanning for system fonts");

    FcInit();
    FcConfig*    config = FcInitLoadConfigAndFonts();
    FcPattern*   pat    = FcPatternCreate();
    FcObjectSet* os     = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_LANG, FC_FILE, (char*)0);
    FcFontSet*   fs     = FcFontList(config, pat, os);
    for (int i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];
        FcChar8 *  file, *style, *family;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
            fonts_[reinterpret_cast<const char*>(family)].variants.emplace(
                reinterpret_cast<const char*>(style),
                font_variant_s{reinterpret_cast<const char*>(style), reinterpret_cast<const char*>(file)});
        }
    }
    if (fs)
        FcFontSetDestroy(fs);

    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    FcConfigDestroy(config);
    FcFini();

    for (const auto& [name, font] : fonts_) {
        log->debug("Found font: {}", name);

        for (const auto& [v_name, variant] : font.variants) {
            log->debug("  --- {}", v_name);
        }
    }
}

font_registry_s::~font_registry_s() {}

const font_s* font_registry_s::find_font(const std::string& name) const
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

std::vector<std::string_view> font_registry_s::get_font_variants(const std::string& name) const
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