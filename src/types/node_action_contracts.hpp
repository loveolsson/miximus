#pragma once
#include "json_contract.hpp"

#include <cstddef>
#include <string_view>
#include <tuple>

namespace miximus {

struct browser_reload_payload_s
{
    bool ignore_cache = false;
};

struct clear_browser_cache_payload_s
{
};

BOOST_DESCRIBE_STRUCT(browser_reload_payload_s, (), (ignore_cache))
BOOST_DESCRIBE_STRUCT(clear_browser_cache_payload_s, (), ())

template <>
inline constexpr bool json_member_defaulted<&browser_reload_payload_s::ignore_cache> = true;

namespace node_actions {
// A contract describes the wire data, not scheduling or handler ownership.
template <typename Payload, typename Result>
struct contract_s
{
    using payload_type = Payload;
    using result_type  = Result;
    std::string_view node_type;
    std::string_view name;
    std::string_view payload_name;
};

inline constexpr contract_s<browser_reload_payload_s, std::nullptr_t>      reload{"cef_browser",
                                                                             "reload",
                                                                             "browser_reload_payload_s"};
inline constexpr contract_s<clear_browser_cache_payload_s, std::nullptr_t> clear_browser_cache{
    "application_settings",
    "clear_browser_cache",
    "clear_browser_cache_payload_s"};
inline constexpr auto contracts = std::tuple{reload, clear_browser_cache};
} // namespace node_actions
} // namespace miximus
