#pragma once
#include "core/node_manager_fwd.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace miximus::core {

class configuration_s
{
    static constexpr uint32_t SCHEMA_VERSION = 1;

    node_manager_s& node_manager_;

    nlohmann::json serialize(bool include_status) const;

  public:
    explicit configuration_s(node_manager_s& node_manager)
        : node_manager_(node_manager)
    {
    }

    void load(nlohmann::json config);
    void load_file(const std::filesystem::path& path);

    nlohmann::json                get_config() const;
    nlohmann::json                get_snapshot() const;
    std::optional<nlohmann::json> get_node(std::string_view id) const;
    std::optional<nlohmann::json> get_node_status(std::string_view id) const;
    void                          save_file(const std::filesystem::path& path) const;
};

} // namespace miximus::core
