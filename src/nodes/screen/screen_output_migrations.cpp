#include "screen_output_migrations.hpp"

#include <nlohmann/json.hpp>

namespace {

void migrate_v1_to_v2(nlohmann::json& options)
{
    options["position"] = nlohmann::json::array({options.value("posx", 0), options.value("posy", 0)});
    options["size"]     = nlohmann::json::array({options.value("sizex", 100), options.value("sizey", 100)});

    options.erase("posx");
    options.erase("posy");
    options.erase("sizex");
    options.erase("sizey");
}

} // namespace

namespace miximus::nodes::screen {

std::vector<node_migration_s> screen_output_migrations()
{
    std::vector<node_migration_s> migrations(1);
    migrations.front().migrate_options = migrate_v1_to_v2;
    return migrations;
}

} // namespace miximus::nodes::screen
