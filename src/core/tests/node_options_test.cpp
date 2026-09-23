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

TEST(NodeOptions, NdiAlphaModesAreValidatedOnBothNodes)
{
    nodes::node_definition_map_t definitions;
    nodes::register_all_nodes(&definitions);
    for (const auto type : {"ndi_input", "ndi_output"}) {
        SCOPED_TRACE(type);
        const auto node = definitions.at(type).constructor();
        EXPECT_EQ(node->get_default_options().at("alpha_mode"), "straight");
        for (const auto mode : {"ignore", "straight", "premultiplied"}) {
            auto       state  = node->get_default_options();
            const auto result = node->set_options(state,
                                                  {
                                                      {"alpha_mode", mode}
            });
            EXPECT_EQ(result.error, error_e::no_error);
            EXPECT_EQ(state.at("alpha_mode"), mode);
        }

        for (const nlohmann::json& invalid : {nlohmann::json("unknown"), nlohmann::json(true), nlohmann::json(1)}) {
            auto value = invalid;
            EXPECT_EQ(node->normalize_option("alpha_mode", &value), nodes::option_result_e::invalid);
        }
    }
}

} // namespace

TEST(NodeOptions, CefBrowserUsesOnePixelSizeAndAlwaysSupportsTransparency)
{
    nodes::node_definition_map_t definitions;
    nodes::register_all_nodes(&definitions);
    const auto node     = definitions.at("cef_browser").constructor();
    const auto defaults = node->get_default_options();
    EXPECT_EQ(defaults.at("size"), nlohmann::json::array({1920, 1080}));
    EXPECT_FALSE(defaults.contains("width"));
    EXPECT_FALSE(defaults.contains("height"));
    EXPECT_FALSE(defaults.contains("transparent"));

    auto size = nlohmann::json::array({640.4, 359.6});
    EXPECT_EQ(node->normalize_option("size", &size), nodes::option_result_e::corrected);
    EXPECT_EQ(size, nlohmann::json::array({640, 360}));
    auto malformed = nlohmann::json::array({640});
    EXPECT_EQ(node->normalize_option("size", &malformed), nodes::option_result_e::invalid);
    auto transparent = nlohmann::json(false);
    EXPECT_EQ(node->normalize_option("transparent", &transparent), nodes::option_result_e::invalid);
}
