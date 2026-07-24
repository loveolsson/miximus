#include "settings_migrations.hpp"

#include <nlohmann/json.hpp>

namespace {

void migrate_v1_to_v2(nlohmann::json& options) { options.erase("decklink_output_preroll_frames"); }

} // namespace

namespace miximus::nodes::system {

std::vector<node_migration_s> settings_migrations()
{
    std::vector<node_migration_s> migrations(1);
    migrations[0].migrate_options = migrate_v1_to_v2;
    return migrations;
}

} // namespace miximus::nodes::system
