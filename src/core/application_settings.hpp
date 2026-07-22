#pragma once
#include "nodes/option_result.hpp"
#include "types/frame_rate.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace miximus::core {

inline constexpr std::string_view APPLICATION_SETTINGS_ID = "$app";

struct application_settings_snapshot_s
{
    frame_rate_s frame_rate;
    uint64_t     revision;
};

class application_settings_s
{
    class impl_s;
    std::unique_ptr<impl_s> impl_;

    static nodes::option_result_e normalize_option(std::string_view name, nlohmann::json* value);

  public:
    application_settings_s();
    ~application_settings_s();

    application_settings_s(const application_settings_s&)            = delete;
    application_settings_s(application_settings_s&&)                 = delete;
    application_settings_s& operator=(const application_settings_s&) = delete;
    application_settings_s& operator=(application_settings_s&&)      = delete;

    nodes::set_options_result_s set_options(const nlohmann::json& options);

    const nlohmann::json&           options() const;
    application_settings_snapshot_s sync_render_snapshot();
};

} // namespace miximus::core
