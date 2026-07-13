#pragma once
#include "gpu/color_transfer.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

namespace miximus::nodes::decklink::detail {

inline gpu::color_transfer_e get_color_transfer(BMDColorspace colorspace)
{
    switch (colorspace) {
        case bmdColorspaceRec601:
            return gpu::color_transfer_e::Rec601;
        case bmdColorspaceRec2020:
            return gpu::color_transfer_e::Rec2020;
        case bmdColorspaceRec709:
        default:
            return gpu::color_transfer_e::Rec709;
    }
}

inline BMDColorspace get_display_mode_colorspace(IDeckLinkDisplayMode* mode)
{
    const auto flags  = mode->GetFlags();
    const bool is_uhd = mode->GetWidth() > 1920 || mode->GetHeight() > 1080;
    const bool is_sd  = mode->GetWidth() <= 720 && mode->GetHeight() <= 576;

    if (is_uhd && (flags & bmdDisplayModeColorspaceRec2020) != 0) {
        return bmdColorspaceRec2020;
    }
    if (is_sd && (flags & bmdDisplayModeColorspaceRec601) != 0) {
        return bmdColorspaceRec601;
    }
    if ((flags & bmdDisplayModeColorspaceRec709) != 0) {
        return bmdColorspaceRec709;
    }
    if ((flags & bmdDisplayModeColorspaceRec2020) != 0) {
        return bmdColorspaceRec2020;
    }

    return is_uhd ? bmdColorspaceRec2020 : (is_sd ? bmdColorspaceRec601 : bmdColorspaceRec709);
}

} // namespace miximus::nodes::decklink::detail
