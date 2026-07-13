#pragma once
#include "nodes/node_fwd.hpp"

#include <nlohmann/json_fwd.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace miximus::nodes {

using constructor_t = std::function<std::shared_ptr<node_i>()>;

struct node_migration_s
{
    std::function<void(nlohmann::json&)> migrate_options;
    std::function<void(std::string&)>    migrate_input_interface;
    std::function<void(std::string&)>    migrate_output_interface;
};

struct node_definition_s
{
    constructor_t                 constructor;
    std::vector<node_migration_s> migrations;

    [[nodiscard]] uint32_t schema_version() const noexcept { return static_cast<uint32_t>(migrations.size()) + 1; }

    template <typename Callable>
        requires std::constructible_from<constructor_t, Callable>
    node_definition_s(Callable&& callable)
        : constructor(std::forward<Callable>(callable))
    {
    }

    node_definition_s(constructor_t constructor_, std::vector<node_migration_s> migrations_)
        : constructor(std::move(constructor_))
        , migrations(std::move(migrations_))
    {
    }
};

using node_definition_map_t = std::map<std::string_view, node_definition_s>;

} // namespace miximus::nodes
