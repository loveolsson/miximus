#pragma once

#include <boost/describe.hpp>

#include <cstdint>

namespace miximus {

// Shared by DeckLink output configuration and status payloads.
enum class decklink_keyer_mode_e : uint8_t
{
    disabled,
    internal,
    external,
};

BOOST_DESCRIBE_ENUM(decklink_keyer_mode_e, disabled, internal, external)

} // namespace miximus
