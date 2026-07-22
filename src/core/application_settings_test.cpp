#include "application_settings.hpp"
#include "types/error.hpp"
#include "types/frame_rate.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <limits>
#include <string_view>

namespace {
using namespace miximus;

bool require(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "Failed: " << description << '\n';
    }
    return condition;
}
} // namespace

int main()
{
    bool success = true;

    const auto integer_duration = get_frame_duration({.numerator = 60, .denominator = 1});
    success &=
        require(integer_duration.has_value() && integer_duration->count() == 11'760'000, "60 fps duration is exact");

    const auto ntsc_duration = get_frame_duration({.numerator = 60'000, .denominator = 1'001});
    success &=
        require(ntsc_duration.has_value() && ntsc_duration->count() == 11'771'760, "60000/1001 fps duration is exact");

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
        success &= require(json.get<frame_rate_s>() == rate, "supported frame rate round-trips through JSON");
    }

    success &= require(canonicalize_frame_rate({.numerator = 120, .denominator = 2}) == DEFAULT_FRAME_RATE,
                       "equivalent rates canonicalize");
    success &=
        require(!get_frame_duration({.numerator = 59, .denominator = 1}).has_value(), "inexact flick rate is rejected");

    core::application_settings_s settings;
    const auto                   initial = settings.sync_render_snapshot();
    success &= require(initial.frame_rate == DEFAULT_FRAME_RATE && initial.revision == 1,
                       "default settings produce initial snapshot");

    const nlohmann::json update{
        {"frame_rate",
         {
             {"numerator", 60'000},
             {"denominator", 1'001},
         }},
    };
    const auto update_result = settings.set_options(update);
    const auto updated       = settings.sync_render_snapshot();
    success &= require(update_result.error == error_e::no_error && !update_result.has_corrected_values,
                       "valid frame rate is accepted");
    success &=
        require(updated.frame_rate == frame_rate_s{.numerator = 60'000, .denominator = 1'001} && updated.revision == 2,
                "updated settings become a new render snapshot");

    const nlohmann::json canonicalized_update{
        {"frame_rate",
         {
             {"numerator", 120},
             {"denominator", 2},
         }},
    };
    const auto canonicalized_result = settings.set_options(canonicalized_update);
    success &= require(canonicalized_result.error == error_e::no_error && canonicalized_result.has_corrected_values,
                       "non-canonical rate reports correction");
    success &= require(settings.options().at("frame_rate").get<frame_rate_s>() == DEFAULT_FRAME_RATE,
                       "corrected rate is stored canonically");

    const auto options_before_invalid = settings.options();
    const auto invalid_result         = settings.set_options({
        {"frame_rate",
         {
             {"numerator", 0},
             {"denominator", 1},
         }},
    });
    success &= require(invalid_result.error == error_e::invalid_options, "invalid frame rate is rejected");
    success &= require(settings.options() == options_before_invalid, "invalid update leaves settings unchanged");

    const auto negative_result = settings.set_options({
        {"frame_rate",
         {
             {"numerator", -60},
             {"denominator", 1},
         }},
    });
    success &= require(negative_result.error == error_e::invalid_options, "negative frame rate is rejected");

    const auto oversized_result = settings.set_options({
        {"frame_rate",
         {
             {"numerator", std::numeric_limits<uint64_t>::max()},
             {"denominator", 1},
         }},
    });
    success &= require(oversized_result.error == error_e::invalid_options, "oversized frame rate is rejected");

    return success ? 0 : 1;
}
