#include "core/application_settings.hpp"
#include "types/error.hpp"
#include "types/frame_rate.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace miximus;

TEST(FrameRate, CommonRatesHaveExactDurationsAndRoundTripThroughJson)
{
    const auto integer_duration = get_frame_duration({.numerator = 60, .denominator = 1});
    ASSERT_TRUE(integer_duration.has_value());
    EXPECT_EQ(integer_duration->count(), 11'760'000);

    const auto ntsc_duration = get_frame_duration({.numerator = 60'000, .denominator = 1'001});
    ASSERT_TRUE(ntsc_duration.has_value());
    EXPECT_EQ(ntsc_duration->count(), 11'771'760);

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

TEST(ApplicationSettings, ProducesImmutableRevisionedSnapshots)
{
    core::application_settings_s settings;
    const auto                   initial = settings.sync_render_snapshot();
    EXPECT_EQ(initial.frame_rate, DEFAULT_FRAME_RATE);
    EXPECT_EQ(initial.revision, 1);

    const nlohmann::json update{
        {"frame_rate",
         {
             {"numerator", 60'000},
             {"denominator", 1'001},
         }},
    };
    const auto update_result = settings.set_options(update);
    const auto updated       = settings.sync_render_snapshot();
    EXPECT_EQ(update_result.error, error_e::no_error);
    EXPECT_FALSE(update_result.has_corrected_values);
    EXPECT_EQ(updated.frame_rate, (frame_rate_s{.numerator = 60'000, .denominator = 1'001}));
    EXPECT_EQ(updated.revision, 2);
}

TEST(ApplicationSettings, ReportsAndStoresCanonicalCorrections)
{
    core::application_settings_s settings;
    const auto                   result = settings.set_options({
        {"frame_rate",
         {
             {"numerator", 120},
             {"denominator", 2},
         }},
    });

    EXPECT_EQ(result.error, error_e::no_error);
    EXPECT_TRUE(result.has_corrected_values);
    EXPECT_EQ(settings.options().at("frame_rate").get<frame_rate_s>(), DEFAULT_FRAME_RATE);
}

TEST(ApplicationSettings, RejectsInvalidUpdatesAtomically)
{
    core::application_settings_s settings;
    const auto                   options_before_invalid = settings.options();

    for (const auto& frame_rate : {
             nlohmann::json{{"numerator", 0},                                    {"denominator", 1}},
             nlohmann::json{{"numerator", -60},                                  {"denominator", 1}},
             nlohmann::json{{"numerator", std::numeric_limits<uint64_t>::max()}, {"denominator", 1}},
    }) {
        const auto result = settings.set_options({
            {"frame_rate", frame_rate}
        });
        EXPECT_EQ(result.error, error_e::invalid_options);
        EXPECT_EQ(settings.options(), options_before_invalid);
    }
}
} // namespace
