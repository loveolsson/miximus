#pragma once

#include <cstddef>

namespace miximus::nodes::cef::detail::command_protocol {
inline constexpr char   CONTEXT_READY[]    = "miximus.cef.context.ready";
inline constexpr char   CONTEXT_RELEASED[] = "miximus.cef.context.released";
inline constexpr char   REQUEST[]          = "miximus.cef.request";
inline constexpr char   RESULT[]           = "miximus.cef.result";
inline constexpr char   CANCEL[]           = "miximus.cef.cancel";
inline constexpr size_t MAX_PENDING        = 64;
inline constexpr size_t MAX_SOURCE_BYTES   = 64 * 1024;
inline constexpr size_t MAX_JSON_BYTES     = 1024 * 1024;
} // namespace miximus::nodes::cef::detail::command_protocol
