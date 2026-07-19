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

void migrate_v2_to_v3(nlohmann::json& options)
{
    options["monitor_id"] = options.value("monitor_name", "");
    options.erase("monitor_name");
}

} // namespace

namespace miximus::nodes::screen {

std::vector<node_migration_s> screen_output_migrations()
{
    std::vector<node_migration_s> migrations(2);
    migrations[0].migrate_options = migrate_v1_to_v2;
    migrations[1].migrate_options = migrate_v2_to_v3;
    return migrations;
}

} // namespace miximus::nodes::screen
