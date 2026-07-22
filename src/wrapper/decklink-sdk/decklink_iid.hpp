#pragma once
#include "decklink_inc.hpp"

#include <DeckLinkAPIVersion.h>
namespace miximus::decklink_sdk {

/**
 * Maps the interface types exposed by DeckLink SDK 16.0 to their matching IIDs.
 *
 * This list is specific to the selected DeckLink API version. When the SDK is
 * updated, review its primary API headers and update this mapping before
 * accepting the new version.
 */
static_assert(BLACKMAGIC_DECKLINK_API_VERSION == 0x10000000, "Review the DeckLink interface-to-IID mappings");

template <typename T>
REFIID decklink_iid() = delete;

#define MIXIMUS_DECKLINK_IID(interface_name)                                                                           \
    template <>                                                                                                        \
    inline REFIID decklink_iid<interface_name>()                                                                       \
    {                                                                                                                  \
        return IID_##interface_name;                                                                                   \
    }

#ifdef _WIN32
MIXIMUS_DECKLINK_IID(IUnknown)
#else
template <>
inline REFIID decklink_iid<IUnknown>()
{
    return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
}
#endif
MIXIMUS_DECKLINK_IID(IDeckLinkTimecode)
MIXIMUS_DECKLINK_IID(IDeckLinkDisplayModeIterator)
MIXIMUS_DECKLINK_IID(IDeckLinkDisplayMode)
MIXIMUS_DECKLINK_IID(IDeckLink)
MIXIMUS_DECKLINK_IID(IDeckLinkConfiguration)
MIXIMUS_DECKLINK_IID(IDeckLinkEncoderConfiguration)
MIXIMUS_DECKLINK_IID(IDeckLinkDeckControlStatusCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkDeckControl)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoOutputCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkInputCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkEncoderInputCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoBufferAllocator)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoBufferAllocatorProvider)
MIXIMUS_DECKLINK_IID(IDeckLinkAudioOutputCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkIterator)
MIXIMUS_DECKLINK_IID(IDeckLinkAPIInformation)
MIXIMUS_DECKLINK_IID(IDeckLinkIPFlowAttributes)
MIXIMUS_DECKLINK_IID(IDeckLinkIPFlowStatus)
MIXIMUS_DECKLINK_IID(IDeckLinkIPFlowSetting)
MIXIMUS_DECKLINK_IID(IDeckLinkIPFlow)
MIXIMUS_DECKLINK_IID(IDeckLinkIPFlowIterator)
MIXIMUS_DECKLINK_IID(IDeckLinkOutput)
MIXIMUS_DECKLINK_IID(IDeckLinkInput)
MIXIMUS_DECKLINK_IID(IDeckLinkIPExtensions)
MIXIMUS_DECKLINK_IID(IDeckLinkHDMIInputEDID)
MIXIMUS_DECKLINK_IID(IDeckLinkEncoderInput)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoBuffer)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoFrame)
MIXIMUS_DECKLINK_IID(IDeckLinkMutableVideoFrame)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoFrame3DExtensions)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoFrameMetadataExtensions)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoFrameMutableMetadataExtensions)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoInputFrame)
MIXIMUS_DECKLINK_IID(IDeckLinkAncillaryPacket)
MIXIMUS_DECKLINK_IID(IDeckLinkAncillaryPacketIterator)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoFrameAncillaryPackets)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoFrameAncillary)
MIXIMUS_DECKLINK_IID(IDeckLinkEncoderPacket)
MIXIMUS_DECKLINK_IID(IDeckLinkEncoderVideoPacket)
MIXIMUS_DECKLINK_IID(IDeckLinkEncoderAudioPacket)
MIXIMUS_DECKLINK_IID(IDeckLinkH265NALPacket)
MIXIMUS_DECKLINK_IID(IDeckLinkAudioInputPacket)
MIXIMUS_DECKLINK_IID(IDeckLinkScreenPreviewCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkGLScreenPreviewHelper)
MIXIMUS_DECKLINK_IID(IDeckLinkNotificationCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkNotification)
MIXIMUS_DECKLINK_IID(IDeckLinkProfileAttributes)
MIXIMUS_DECKLINK_IID(IDeckLinkProfileIterator)
MIXIMUS_DECKLINK_IID(IDeckLinkProfile)
MIXIMUS_DECKLINK_IID(IDeckLinkProfileCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkProfileManager)
MIXIMUS_DECKLINK_IID(IDeckLinkStatistics)
MIXIMUS_DECKLINK_IID(IDeckLinkStatus)
MIXIMUS_DECKLINK_IID(IDeckLinkKeyer)
MIXIMUS_DECKLINK_IID(IDeckLinkVideoConversion)
MIXIMUS_DECKLINK_IID(IDeckLinkDeviceNotificationCallback)
MIXIMUS_DECKLINK_IID(IDeckLinkDiscovery)

#ifdef _WIN32
MIXIMUS_DECKLINK_IID(IDeckLinkDX9ScreenPreviewHelper)
MIXIMUS_DECKLINK_IID(IDeckLinkWPFDX9ScreenPreviewHelper)
#elif defined(__APPLE__)
MIXIMUS_DECKLINK_IID(IDeckLinkMetalScreenPreviewHelper)
#endif

#undef MIXIMUS_DECKLINK_IID

} // namespace miximus::decklink_sdk
