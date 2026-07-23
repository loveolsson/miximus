#include "core/app_state.hpp"
#include "nodes/node.hpp"
#include "nodes/system/register.hpp"
#include "types/error.hpp"
#include "types/frame_rate.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
using namespace miximus;

template <typename T>
T require_value(std::optional<T> value)
{
    if (!value.has_value()) {
        throw std::logic_error("test expected an optional value");
    }
    return std::move(value).value();
}

std::shared_ptr<nodes::node_i> create_settings_node()
{
    nodes::node_definition_map_t definitions;
    nodes::system::register_nodes(&definitions);
    return definitions.at(nodes::system::SETTINGS_NODE_TYPE).constructor();
}

TEST(FrameRate, CommonRatesHaveExactDurationsAndRoundTripThroughJson)
{
    const auto integer_duration = require_value(get_frame_duration({.numerator = 60, .denominator = 1}));
    EXPECT_EQ(integer_duration.count(), 11'760'000);

    const auto ntsc_duration = require_value(get_frame_duration({.numerator = 60'000, .denominator = 1'001}));
    EXPECT_EQ(ntsc_duration.count(), 11'771'760);

    for (const auto rate : {
             frame_rate_s{.numerator = 24,     .denominator = 1    },
             frame_rate_s{.numerator = 25,     .denominator = 1    },
             frame_rate_s{.numerator = 30,     .denominator = 1    },
             frame_rate_s{.numerator = 50,     .denominator = 1    },
             frame_rate_s{.numerator = 60,     .denominator = 1    },
             frame_rate_s{.numerator = 24'000, .denominator = 1'001},
             frame_rate_s{.numerator = 30'000, .denominator = 1'001},
             frame_rate_s{.numerator = 60'000, .denominator = 1'001},
    }) {
        const nlohmann::json json = rate;
        EXPECT_EQ(json.get<frame_rate_s>(), rate);
    }
}

TEST(FrameRate, CanonicalizesEquivalentRatesAndRejectsInexactRates)
{
    EXPECT_EQ(canonicalize_frame_rate({.numerator = 120, .denominator = 2}), DEFAULT_FRAME_RATE);
    EXPECT_FALSE(get_frame_duration({.numerator = 59, .denominator = 1}).has_value());
}

TEST(SettingsNode, ProvidesTheDefaultSettings)
{
    using decklink_output_settings_s = core::app_state_s::frame_settings_s::decklink_output_settings_s;

    const auto settings = create_settings_node();
    const auto defaults = settings->get_default_options();
    ASSERT_TRUE(defaults.contains("frame_rate"));
    EXPECT_EQ(defaults.at("frame_rate").get<frame_rate_s>(), DEFAULT_FRAME_RATE);
    EXPECT_EQ(defaults.at("decklink_output_preroll_frames").get<int>(),
              decklink_output_settings_s::DEFAULT_PREROLL_FRAMES);
    EXPECT_EQ(defaults.at("decklink_output_buffer_frames").get<int>(),
              decklink_output_settings_s::DEFAULT_BUFFER_FRAMES);
}

TEST(SettingsNode, CorrectsDeckLinkOutputBufferSettings)
{
    using decklink_output_settings_s = core::app_state_s::frame_settings_s::decklink_output_settings_s;

    const auto           settings = create_settings_node();
    auto                 state    = settings->get_default_options();
    const nlohmann::json update{
        {"decklink_output_preroll_frames", 0  },
        {"decklink_output_buffer_frames",  100},
    };
    const auto result = settings->set_options(state, update);

    EXPECT_EQ(result.error, error_e::no_error);
    EXPECT_TRUE(result.has_corrected_values);
    EXPECT_EQ(state.at("decklink_output_preroll_frames").get<int>(), decklink_output_settings_s::MIN_BUFFER_FRAMES);
    EXPECT_EQ(state.at("decklink_output_buffer_frames").get<int>(), decklink_output_settings_s::MAX_BUFFER_FRAMES);
}

TEST(SettingsNode, ReportsAndStoresCanonicalCorrections)
{
    const auto           settings = create_settings_node();
    auto                 state    = settings->get_default_options();
    const nlohmann::json update{
        {"frame_rate",
         {
             {"numerator", 120},
             {"denominator", 2},
         }},
    };
    const auto result = settings->set_options(state, update);

    EXPECT_EQ(result.error, error_e::no_error);
    EXPECT_TRUE(result.has_corrected_values);
    EXPECT_EQ(state.at("frame_rate").get<frame_rate_s>(), DEFAULT_FRAME_RATE);
}

TEST(SettingsNode, RejectsInvalidUpdatesAtomically)
{
    const auto settings = create_settings_node();
    auto       state    = settings->get_default_options();

    for (const auto& frame_rate : {
             nlohmann::json{{"numerator", 0},                                    {"denominator", 1}},
             nlohmann::json{{"numerator", -60},                                  {"denominator", 1}},
             nlohmann::json{{"numerator", std::numeric_limits<uint64_t>::max()}, {"denominator", 1}},
    }) {
        const auto           state_before = state;
        const nlohmann::json update{
            {"frame_rate", frame_rate},
        };
        const auto result = settings->set_options(state, update);
        EXPECT_EQ(result.error, error_e::invalid_options);
        EXPECT_EQ(state, state_before);
    }
}

} // namespace
