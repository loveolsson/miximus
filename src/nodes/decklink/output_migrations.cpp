#include "output_migrations.hpp"

#include <nlohmann/json.hpp>

namespace {

void migrate_v1_to_v2(nlohmann::json& options) { options["keyer_mode"] = "disabled"; }

} // namespace

namespace miximus::nodes::decklink {

std::vector<node_migration_s> output_migrations()
{
    std::vector<node_migration_s> migrations(1);
    migrations[0].migrate_options = migrate_v1_to_v2;
    return migrations;
}

} // namespace miximus::nodes::decklink
