#pragma once

#include <boost/describe.hpp>

#include <cstdint>

namespace miximus {

// Shared by browser sessions and status payloads, including states with no session.
enum class cef_state_e : uint8_t
{
    starting,
    loading,
    ready,
    closing,
    closed,
    failed,
    stopped,
    unavailable,
};

enum class cef_input_state_e : uint8_t
{
    unavailable,
    idle,
    active,
    failed,
};

BOOST_DESCRIBE_ENUM(cef_state_e, starting, loading, ready, closing, closed, failed, stopped, unavailable)
BOOST_DESCRIBE_ENUM(cef_input_state_e, unavailable, idle, active, failed)

} // namespace miximus
