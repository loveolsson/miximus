#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/register_all.hpp"
#include "types/error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>

namespace {
using namespace miximus;

enum class test_mode_e : std::uint8_t
{
    active,
};

TEST(NodeOptions, RegisteredDefaultsAreValidAndCanonical)
{
    nodes::node_definition_map_t definitions;
    nodes::register_all_nodes(&definitions);

    for (const auto& [type, definition] : definitions) {
        const auto node     = definition.constructor();
        const auto defaults = node->get_default_options();
        auto       state    = nlohmann::json::object();

        const auto result = node->set_options(state, defaults);
        EXPECT_EQ(result.error, error_e::no_error) << type;
        EXPECT_FALSE(result.has_corrected_values) << type;
        EXPECT_EQ(state, defaults) << type;
    }
}

TEST(NodeOptions, ReadsValidatedEnumWithoutFallback)
{
    nodes::node_state_s state;
    state.options = {
        {"mode", "active"},
    };

    EXPECT_EQ(state.get_enum_option_unchecked<test_mode_e>("mode"), test_mode_e::active);
}

} // namespace
