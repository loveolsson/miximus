#include "core/node_status_registry.hpp"
#include "types/node_status_json.hpp"

#include <array>
#include <chrono>
#include <gtest/gtest.h>
#include <utility>

namespace miximus::core::tests {

struct unregistered_status_s
{
    bool connected{};
};
BOOST_DESCRIBE_STRUCT(unregistered_status_s, (), (connected))

template <typename T>
concept publishable_status =
    requires(node_status_registry_s& registry, const T& value) { registry.write("node", value); };

static_assert(publishable_status<status::connected_status_s>);
static_assert(!publishable_status<unregistered_status_s>);
static_assert(!publishable_status<nlohmann::json>);

TEST(node_status_registry, cef_states_serialize_as_existing_wire_names)
{
    node_status_registry_s       registry;
    status::cef_browser_status_s payload;
    const std::array             states{
        std::pair{cef_state_e::starting,    "starting"   },
        std::pair{cef_state_e::loading,     "loading"    },
        std::pair{cef_state_e::ready,       "ready"      },
        std::pair{cef_state_e::closing,     "closing"    },
        std::pair{cef_state_e::closed,      "closed"     },
        std::pair{cef_state_e::failed,      "failed"     },
        std::pair{cef_state_e::stopped,     "stopped"    },
        std::pair{cef_state_e::unavailable, "unavailable"},
    };
    for (const auto& [state, name] : states) {
        payload.cef_state = state;
        registry.write("browser", payload);
        EXPECT_EQ(registry.get("browser").at("cef_state"), name);
        EXPECT_EQ(nlohmann::json(name).get<cef_state_e>(), state);
    }

    const std::array input_states{
        std::pair{cef_input_state_e::unavailable, "unavailable"},
        std::pair{cef_input_state_e::idle,        "idle"       },
        std::pair{cef_input_state_e::active,      "active"     },
        std::pair{cef_input_state_e::failed,      "failed"     },
    };
    for (const auto& [state, name] : input_states) {
        payload.cef_inputs_state = state;
        registry.write("browser", payload);
        EXPECT_EQ(registry.get("browser").at("cef_inputs_state"), name);
        EXPECT_EQ(nlohmann::json(name).get<cef_input_state_e>(), state);
    }

    EXPECT_THROW(nlohmann::json("invalid").get<cef_state_e>(), std::invalid_argument);
    EXPECT_THROW(nlohmann::json("invalid").get<cef_input_state_e>(), std::invalid_argument);
}

TEST(node_status_registry, described_status_is_serialized_and_delta_filtered)
{
    node_status_registry_s         registry;
    status::source_timing_status_s source_status;
    source_status.source_queue_pushed    = 1;
    source_status.source_recovered_rate  = 59.94;
    source_status.source_phase_offset_us = utils::flicks_cast(std::chrono::microseconds{125});
    registry.write("decklink", source_status);

    EXPECT_EQ(registry.get("decklink"),
              nlohmann::json({
                  {"source_queue_pushed",                  1      },
                  {"source_queue_depth",                   0      },
                  {"source_queue_overflow_drops",          0      },
                  {"source_queue_selection_drops",         0      },
                  {"source_queue_repeated",                0      },
                  {"source_queue_starvation_repeats",      0      },
                  {"source_queue_timing_repeats",          0      },
                  {"source_queue_missing",                 0      },
                  {"source_queue_discontinuities",         0      },
                  {"source_queue_transfer_failures",       0      },
                  {"source_queue_transfer_cancellations",  0      },
                  {"source_recovered_rate",                59.94  },
                  {"source_observed_rate",                 nullptr},
                  {"source_phase_offset_us",               125    },
                  {"source_phase_error_us",                nullptr},
                  {"source_phase_adjustment_us",           nullptr},
                  {"source_repeat_next_frame_lead_min_us", nullptr},
                  {"source_repeat_next_frame_lead_max_us", nullptr},
    }));

    auto updates = registry.flush();
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(updates.front().status, registry.get("decklink"));

    source_status.source_queue_pushed = 2;
    registry.write("decklink", source_status);

    updates = registry.flush();
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(updates.front().status,
              nlohmann::json({
                  {"source_queue_pushed", 2},
    }));

    registry.write("decklink", source_status);
    EXPECT_TRUE(registry.flush().empty());
}

TEST(node_status_registry, typed_status_groups_are_merged_into_node_status)
{
    node_status_registry_s registry;
    registry.write("output", status::connected_status_s{.connected = true});
    registry.write("output",
                   status::display_modes_status_s{
                       .display_modes = {{.id = "1080p60", .label = "1080p60"}},
                   });

    EXPECT_EQ(registry.get("output"),
              nlohmann::json({
                  {"connected",     true                                       },
                  {"display_modes", {{{"id", "1080p60"}, {"label", "1080p60"}}}},
    }));
    ASSERT_EQ(registry.flush().size(), 1);
}

} // namespace miximus::core::tests
