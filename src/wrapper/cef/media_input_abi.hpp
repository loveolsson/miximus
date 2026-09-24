#pragma once

#include <cstdint>

namespace miximus::cef_wrapper {

// Opt-in prototype ABI, versioned independently of stock CEF's generated API.
// Must match media-input-prototype/cef.patch's cef_miximus_media_input.h.
struct media_frame_s
{
    uint32_t size{sizeof(media_frame_s)};
    uint32_t input{};
    int      fd{-1};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    uint64_t offset{};
    uint64_t modifier{};
    uint64_t allocation_bytes{};
    int64_t  timestamp_us{};
    // Monotonic within this document/input. Older in-flight copies may retire
    // safely but cannot enter the native media source after generation advances.
    uint64_t source_generation{1};
    // Advance without a texture: fd=-1 and zero dimensions. Does not retire GPU work.
    uint32_t invalidate_only{};
};
using media_done_t           = void (*)(void* user, int safe_to_reuse, int delivered);
using install_media_inputs_t = int (*)(const char* document_token, uint32_t destination_depth);
using send_media_frame_t =
    int (*)(int browser_id, const char* document_token, const media_frame_s* frame, media_done_t done, void* user);

inline constexpr auto INSTALL_MEDIA_INPUTS = "cef_miximus_install_media_inputs_v1";
inline constexpr auto SEND_MEDIA_FRAME     = "cef_miximus_send_media_frame_v2";

} // namespace miximus::cef_wrapper
