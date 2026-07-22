#pragma once
#include "core/node_manager_fwd.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>

namespace miximus::core {

class configuration_s
{
    static constexpr uint32_t SCHEMA_VERSION = 2;

    node_manager_s& node_manager_;

    nlohmann::json serialize(bool include_status) const;

  public:
    explicit configuration_s(node_manager_s& node_manager)
        : node_manager_(node_manager)
    {
    }

    void load(nlohmann::json config);
    void load_file(const std::filesystem::path& path);

    nlohmann::json get_config() const;
    nlohmann::json get_snapshot() const;
    void           save_file(const std::filesystem::path& path) const;
};

} // namespace miximus::core
