#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>

namespace miximus::node_action_limits {
inline constexpr size_t MAX_PAYLOAD_BYTES = 64 * 1024;
inline constexpr size_t MAX_JSON_DEPTH    = 64;
inline constexpr size_t MAX_JSON_VALUES   = 16384;
inline constexpr size_t MAX_ID_BYTES      = 256;
inline constexpr size_t MAX_NAME_BYTES    = 128;
inline constexpr size_t MAX_TOKEN_BYTES   = 256;

// Validate before copying an untrusted JSON value or recursively serializing it.
bool valid_payload(const nlohmann::json& payload);
} // namespace miximus::node_action_limits
