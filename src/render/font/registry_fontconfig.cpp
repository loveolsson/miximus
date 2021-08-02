#include "logger/logger.hpp"
#include "registry.hpp"

#include <fontconfig/fontconfig.h>

namespace miximus::render::font {

font_registry_s::font_registry_s()
{
    auto log = getlog("app");
    log->debug("Scanning for system fonts");

    FcInit();
    auto* config_ = FcInitLoadConfigAndFonts();
    auto* pat_    = FcPatternCreate();
    auto* os_     = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_LANG, FC_FILE, (char*)0);
    auto* fs_     = FcFontList(config_, pat_, os_);
    for (int i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs_->fonts[i];
        FcChar8 *  file, *style, *family;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
            fonts_[reinterpret_cast<const char*>(family)].variants.emplace(
                reinterpret_cast<const char*>(style),
                font_variant_s{reinterpret_cast<const char*>(style), reinterpret_cast<const char*>(file)});
        }
    }

    log_fonts();

    if (fs_) {
        FcFontSetDestroy(fs_);
    }

    if (os_) {
        FcObjectSetDestroy(os_);
    }

    if (pat_) {
        FcPatternDestroy(pat_);
    }

    if (config_) {
        FcConfigDestroy(config_);
    }

    FcFini();
}

} // namespace miximus::render::font