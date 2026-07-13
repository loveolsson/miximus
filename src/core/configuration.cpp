#include "configuration.hpp"

#include "core/node_manager.hpp"
#include "core/node_status_registry.hpp"
#include "logger/logger.hpp"
#include "nodes/connection.hpp"
#include "nodes/node.hpp"
#include "utils/lookup.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
using namespace miximus;
using nlohmann::json;

constexpr uint32_t LEGACY_SCHEMA_VERSION = 1;

uint32_t get_schema_version(const json& object, std::string_view description)
{
    const auto version_it = object.find("schema_version");
    if (version_it == object.end()) {
        return LEGACY_SCHEMA_VERSION;
    }

    if (!version_it->is_number_integer()) {
        throw std::runtime_error(std::format("{} schema_version must be an integer", description));
    }

    const auto version = version_it->get<int64_t>();
    if (version < 1 || version > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::format("{} schema_version {} is invalid", description, version));
    }

    return static_cast<uint32_t>(version);
}

struct node_load_info_s
{
    const miximus::nodes::node_definition_s* definition;
    std::string                              type;
    uint32_t                                 original_version;
};

const miximus::nodes::node_migration_s&
get_migration(const node_load_info_s& info, uint32_t version, std::string_view type)
{
    if (version == 0 || version > info.definition->migrations.size()) {
        throw std::runtime_error(std::format("Node type {} has no migration from schema version {}", type, version));
    }
    return info.definition->migrations[version - 1];
}

void require_no_error(error_e error, std::string_view operation, std::string_view id)
{
    if (error != error_e::no_error) {
        throw std::runtime_error(std::format("Failed to {} {}: {}", operation, id, enum_to_string(error)));
    }
}
} // namespace

namespace miximus::core {

void configuration_s::load(json config)
{
    const auto document_version = get_schema_version(config, "Configuration");
    if (document_version != SCHEMA_VERSION) {
        throw std::runtime_error(std::format(
            "Configuration schema version {} is not supported; expected {}", document_version, SCHEMA_VERSION));
    }
    config["schema_version"] = SCHEMA_VERSION;

    {
        const std::unique_lock lock(node_manager_.nodes_mutex_);
        if (!node_manager_.nodes_.empty() || !node_manager_.connections_.empty()) {
            throw std::logic_error("Cannot load configuration into a non-empty node manager");
        }
    }

    auto& nodes       = config.at("nodes");
    auto& connections = config.at("connections");
    if (!nodes.is_array() || !connections.is_array()) {
        throw std::runtime_error("Configuration nodes and connections must be arrays");
    }

    std::unordered_map<std::string, node_load_info_s> node_info;
    node_info.reserve(nodes.size());

    for (auto& node : nodes) {
        const auto type = node.at("type").get<std::string_view>();
        const auto id   = node.at("id").get<std::string>();

        const auto definition = node_manager_.node_definitions_.find(type);
        if (definition == node_manager_.node_definitions_.end()) {
            throw std::runtime_error(std::format("Node {} has unknown type {}", id, type));
        }

        const auto original_version = get_schema_version(node, std::format("Node {} ({})", id, type));
        const auto current_version  = definition->second.schema_version();
        if (original_version > current_version) {
            throw std::runtime_error(
                std::format("Node {} ({}) uses schema version {}, but this application supports {}",
                            id,
                            type,
                            original_version,
                            current_version));
        }

        const node_load_info_s info{&definition->second, std::string(type), original_version};
        auto&                  options = node.at("options");
        for (auto version = original_version; version < current_version; ++version) {
            const auto& migration = get_migration(info, version, type);
            if (migration.migrate_options) {
                migration.migrate_options(options);
            }
        }
        node["schema_version"] = current_version;

        if (!node_info.emplace(id, info).second) {
            throw std::runtime_error(std::format("Duplicate node id {}", id));
        }
    }

    for (auto& connection : connections) {
        const auto from_node = connection.at("from_node").get<std::string>();
        const auto to_node   = connection.at("to_node").get<std::string>();

        const auto from_info = node_info.find(from_node);
        const auto to_info   = node_info.find(to_node);
        if (from_info == node_info.end() || to_info == node_info.end()) {
            throw std::runtime_error(std::format("Connection references missing node {} -> {}", from_node, to_node));
        }

        auto from_interface = connection.at("from_interface").get<std::string>();
        auto to_interface   = connection.at("to_interface").get<std::string>();

        for (auto version = from_info->second.original_version;
             version < from_info->second.definition->schema_version();
             ++version) {
            const auto& migration = get_migration(from_info->second, version, from_info->second.type);
            if (migration.migrate_output_interface) {
                migration.migrate_output_interface(from_interface);
            }
        }
        for (auto version = to_info->second.original_version; version < to_info->second.definition->schema_version();
             ++version) {
            const auto& migration = get_migration(to_info->second, version, to_info->second.type);
            if (migration.migrate_input_interface) {
                migration.migrate_input_interface(to_interface);
            }
        }

        connection["from_interface"] = std::move(from_interface);
        connection["to_interface"]   = std::move(to_interface);
    }

    for (const auto& node : nodes) {
        const auto type = node.at("type").get<std::string_view>();
        const auto id   = node.at("id").get<std::string_view>();
        require_no_error(node_manager_.handle_add_node(type, id, node.at("options"), -1), "create node", id);
    }

    for (const auto& connection : connections) {
        const auto parsed = connection.get<nodes::connection_s>();
        require_no_error(
            node_manager_.handle_add_connection(parsed, -1), "create connection from node", parsed.from_node);
    }
}

void configuration_s::load_file(const std::filesystem::path& path)
{
    auto log = getlog("app");
    log->info("Reading settings from {}", path.string());

    std::ifstream file(path);
    if (!file.is_open()) {
        log->error("Failed to open settings file {}", path.string());
        return;
    }

    json config;
    try {
        config = json::parse(file);
    } catch (const json::parse_error& error) {
        throw std::runtime_error(std::format("Failed to parse settings file {}: {}", path.string(), error.what()));
    }

    load(std::move(config));
}

json configuration_s::serialize(bool include_status) const
{
    const std::unique_lock lock(node_manager_.nodes_mutex_);

    auto nodes       = json::array();
    auto connections = json::array();

    for (const auto& [id, record] : node_manager_.nodes_) {
        const auto definition = node_manager_.node_definitions_.find(record.node->type());
        if (definition == node_manager_.node_definitions_.end()) {
            throw std::logic_error(std::format("Node type {} is not registered", record.node->type()));
        }

        nodes.emplace_back(json{
            {"id",             id                                 },
            {"type",           record.node->type()                },
            {"schema_version", definition->second.schema_version()},
            {"options",        record.state.options               },
        });
    }

    for (const auto& connection : node_manager_.connections_) {
        connections.emplace_back(connection);
    }

    json result{
        {"schema_version", SCHEMA_VERSION        },
        {"nodes",          std::move(nodes)      },
        {"connections",    std::move(connections)},
    };

    if (include_status) {
        result["status"] =
            node_manager_.status_registry_ != nullptr ? node_manager_.status_registry_->get_all() : json::object();
    }

    return result;
}

json configuration_s::get_config() const { return serialize(false); }

json configuration_s::get_snapshot() const { return serialize(true); }

void configuration_s::save_file(const std::filesystem::path& path) const
{
    auto log = getlog("app");
    log->info("Writing settings to {}", path.string());

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(std::format("Failed to open settings file {} for writing", path.string()));
    }

    file << std::setfill(' ') << std::setw(2) << get_config();
    if (!file) {
        throw std::runtime_error(std::format("Failed to write settings file {}", path.string()));
    }
}

} // namespace miximus::core
