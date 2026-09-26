#include "core/node_status_registry.hpp"
#include "types/node_status_json.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <thread>
#include <utility>

namespace miximus::core::tests {

struct unregistered_status_s
{
    bool connected{};
};
BOOST_DESCRIBE_STRUCT(unregistered_status_s, (), (connected))

template <typename T>
concept publishable_status =
    requires(node_status_registry_s& registry, const T& value) { registry.write(node_status_handle_s("node"), value); };

static_assert(publishable_status<status::connected_status_s>);
static_assert(!publishable_status<unregistered_status_s>);
static_assert(!publishable_status<nlohmann::json>);

struct node_status_registry_test_s : testing::Test
{
    boost::asio::io_context                              executor;
    node_status_registry_s                               registry{executor, std::chrono::milliseconds(20)};
    node_status_handle_s                                 decklink{"decklink"};
    node_status_handle_s                                 browser{"browser"};
    node_status_handle_s                                 output{"output"};
    std::vector<node_status_registry_s::status_update_s> updates;
    size_t                                               callbacks{};

    node_status_registry_test_s()
    {
        registry.set_callback([this](const auto& batch) {
            EXPECT_TRUE(executor.get_executor().running_in_this_thread());
            ++callbacks;
            updates.insert(updates.end(), batch.begin(), batch.end());
        });
    }
    auto flush()
    {
        registry.publish();
        executor.restart();
        executor.run();
        return std::exchange(updates, {});
    }
};

TEST_F(node_status_registry_test_s, keyer_modes_serialize_as_existing_wire_names)
{
    for (const auto [mode, name] : std::array{
             std::pair{decklink_keyer_mode_e::disabled, "disabled"},
             std::pair{decklink_keyer_mode_e::internal, "internal"},
             std::pair{decklink_keyer_mode_e::external, "external"},
    }) {
        registry.write(decklink,
                       status::decklink_output_keyer_status_s{
                           .requested_keyer_mode  = mode,
                           .active_keyer_mode     = mode,
                           .keyer_fallback_reason = std::nullopt,
                       });
        flush();
        EXPECT_EQ(registry.get("decklink").at("requested_keyer_mode"), name);
        EXPECT_EQ(registry.get("decklink").at("active_keyer_mode"), name);
        EXPECT_TRUE(registry.get("decklink").at("keyer_fallback_reason").is_null());
        EXPECT_EQ(nlohmann::json(name).get<decklink_keyer_mode_e>(), mode);
    }
    EXPECT_THROW(nlohmann::json("invalid").get<decklink_keyer_mode_e>(), std::invalid_argument);
}

TEST_F(node_status_registry_test_s, cef_states_serialize_as_existing_wire_names)
{
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
        registry.write(browser, payload);
        flush();
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
        registry.write(browser, payload);
        flush();
        EXPECT_EQ(registry.get("browser").at("cef_inputs_state"), name);
        EXPECT_EQ(nlohmann::json(name).get<cef_input_state_e>(), state);
    }

    EXPECT_THROW(nlohmann::json("invalid").get<cef_state_e>(), std::invalid_argument);
    EXPECT_THROW(nlohmann::json("invalid").get<cef_input_state_e>(), std::invalid_argument);
}

TEST_F(node_status_registry_test_s, described_status_is_serialized_and_delta_filtered)
{
    status::source_timing_status_s source_status;
    source_status.source_queue_pushed    = 1;
    source_status.source_recovered_rate  = 59.94;
    source_status.source_phase_offset_us = utils::flicks_cast(std::chrono::microseconds{125});
    registry.write(decklink, source_status);
    auto result = flush();

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

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front().status, registry.get("decklink"));

    source_status.source_queue_pushed = 2;
    registry.write(decklink, source_status);

    result = flush();
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front().status,
              nlohmann::json({
                  {"source_queue_pushed", 2},
    }));

    registry.write(decklink, source_status);
    EXPECT_TRUE(flush().empty());
}

TEST_F(node_status_registry_test_s, typed_status_groups_are_merged_into_node_status)
{
    registry.write(output, status::connected_status_s{.connected = true});
    registry.write(output,
                   status::display_modes_status_s{
                       .display_modes = {{.id = "1080p60", .label = "1080p60"}},
                   });

    auto result = flush();
    EXPECT_EQ(registry.get("output"),
              nlohmann::json({
                  {"connected",     true                                       },
                  {"display_modes", {{{"id", "1080p60"}, {"label", "1080p60"}}}},
    }));
    ASSERT_EQ(result.size(), 1);
}

TEST_F(node_status_registry_test_s, waits_for_frame_publication_and_executor)
{
    registry.write(output, status::connected_status_s{.connected = true});
    EXPECT_TRUE(registry.get("output").empty());
    EXPECT_EQ(executor.poll(), 0);
    registry.publish();
    EXPECT_TRUE(registry.get("output").empty());
    executor.restart();
    EXPECT_EQ(executor.run(), 1);
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(registry.get("output").at("connected"), true);
}

TEST_F(node_status_registry_test_s, stalled_executor_coalesces_groups_without_losing_catalogues)
{
    registry.write(output, status::display_modes_status_s{.display_modes = {{.id = "mode", .label = "Mode"}}});
    for (uint64_t i = 0; i != 1000; ++i) {
        registry.write(output, status::connected_status_s{.connected = i % 2 != 0});
        registry.write(output, status::ndi_output_metrics_status_s{.frames_sent = i});
        registry.publish();
    }
    EXPECT_EQ(executor.run(), 1);
    EXPECT_EQ(callbacks, 1);
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(registry.get("output").at("frames_sent"), 999);
    EXPECT_EQ(registry.get("output").at("connected"), true);
    EXPECT_EQ(registry.get("output").at("display_modes").size(), 1);
}

TEST_F(node_status_registry_test_s, owns_values_and_clears_optional_fields)
{
    status::decklink_output_keyer_status_s payload{.keyer_fallback_reason = "initial"};
    registry.write(output, payload);
    payload.keyer_fallback_reason = "changed by producer";
    flush();
    EXPECT_EQ(registry.get("output").at("keyer_fallback_reason"), "initial");
    payload.keyer_fallback_reason.reset();
    registry.write(output, std::move(payload));
    const auto result = flush();
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front().status,
              nlohmann::json({
                  {"keyer_fallback_reason", nullptr}
    }));
}

TEST_F(node_status_registry_test_s, removal_discards_staged_and_queued_old_instances)
{
    registry.write(output, status::connected_status_s{.connected = true});
    flush();
    registry.write(output, status::connected_status_s{.connected = false});
    registry.publish();
    registry.write(output, status::screen_output_status_s{.screen_error = "staged"});
    registry.remove_node(output);
    EXPECT_TRUE(registry.get_all().empty());
    const node_status_handle_s replacement("output");
    registry.write(replacement, status::screen_output_status_s{.screen_error = "replacement"});
    registry.write(output, status::connected_status_s{.connected = true});
    const auto result = flush();
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(registry.get("output"),
              nlohmann::json({
                  {"screen_error", "replacement"}
    }));
    registry.remove_node(output); // A late retirement must not erase the replacement.
    EXPECT_EQ(registry.get("output").at("screen_error"), "replacement");
}

TEST_F(node_status_registry_test_s, overlapping_contract_fields_keep_last_write_order)
{
    registry.write(output, status::decklink_input_metrics_status_s{.frames_received = 1});
    registry.publish();
    registry.write(output, status::ndi_input_metrics_status_s{.frames_received = 2});
    registry.write(output, status::decklink_input_metrics_status_s{.frames_received = 3});
    flush();
    EXPECT_EQ(registry.get("output").at("frames_received"), 3);
}

TEST_F(node_status_registry_test_s, concurrent_publication_reschedules_and_yields_to_commands)
{
    // Publish from the producer while the consumer is inside its first callback.
    std::promise<void> entered;
    std::promise<void> resume;
    auto               ready       = resume.get_future();
    size_t             received    = 0;
    bool               command_ran = false;
    registry.set_callback([&](const auto&) {
        EXPECT_TRUE(executor.get_executor().running_in_this_thread());
        if (++received == 1) {
            entered.set_value();
            ready.wait();
        } else {
            EXPECT_TRUE(command_ran);
        }
    });
    registry.write(output, status::connected_status_s{.connected = true});
    registry.publish();
    std::thread consumer([&] { executor.run(); });
    entered.get_future().wait();
    boost::asio::post(executor, [&] { command_ran = true; });
    registry.write(output, status::connected_status_s{.connected = false});
    registry.publish();
    resume.set_value();
    consumer.join();
    EXPECT_EQ(received, 2);
    EXPECT_EQ(registry.get("output").at("connected"), false);
}

TEST_F(node_status_registry_test_s, serialization_is_deferred_and_bad_group_does_not_block_later_batches)
{
    // Deliberately invalid: verify deferred serialization failure is isolated.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid = static_cast<decklink_keyer_mode_e>(-1);
    EXPECT_NO_THROW(registry.write(output,
                                   status::decklink_output_keyer_status_s{.requested_keyer_mode  = invalid,
                                                                          .keyer_fallback_reason = std::nullopt}));
    registry.write(output, status::connected_status_s{.connected = true});
    auto result = flush();
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front().status,
              nlohmann::json({
                  {"connected", true}
    }));
    registry.write(output, status::connected_status_s{.connected = false});
    result = flush();
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front().status,
              nlohmann::json({
                  {"connected", false}
    }));
}

TEST_F(node_status_registry_test_s, stop_and_destruction_cancel_queued_callbacks)
{
    registry.write(output, status::connected_status_s{.connected = true});
    registry.publish();
    registry.stop();
    executor.run();
    EXPECT_EQ(callbacks, 0);
    executor.restart();
    {
        node_status_registry_s temporary(executor);
        temporary.set_callback([&](const auto&) { ADD_FAILURE() << "Destroyed registry published"; });
        temporary.write(output, status::connected_status_s{.connected = true});
        temporary.publish();
    }
    executor.run();
}

TEST_F(node_status_registry_test_s, normal_updates_and_pulls_do_not_consume_the_broadcast_delta)
{
    registry.write(output, status::ndi_output_metrics_status_s{.frames_sent = 1});
    flush();
    registry.write(output, status::ndi_output_metrics_status_s{.frames_sent = 2});
    registry.publish();
    executor.restart();
    ASSERT_EQ(executor.poll_one(), 1); // Admit typed values, leaving the timer pending.
    EXPECT_TRUE(updates.empty());
    EXPECT_EQ(registry.get("output").at("frames_sent"), 2);
    EXPECT_EQ(registry.get_all().at("output").at("frames_sent"), 2);
    executor.run(); // No further producer write: the timer must deliver the change.
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(updates.front().status.at("frames_sent"), 2);
}

TEST_F(node_status_registry_test_s, immediate_delivery_survives_frame_and_mailbox_coalescing)
{
    registry.write(output, status::connected_status_s{.connected = false});
    flush();
    registry.write(output, status::connected_status_s{.connected = true}, status_delivery_e::immediate);
    registry.write(output, status::connected_status_s{.connected = true});
    registry.publish();
    registry.write(output, status::connected_status_s{.connected = true});
    registry.write(output, status::ndi_output_metrics_status_s{.frames_sent = 42});
    registry.publish();
    executor.restart();
    ASSERT_EQ(executor.poll_one(), 1);
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(updates.front().status.at("connected"), true);
    EXPECT_EQ(updates.front().status.at("frames_sent"), 42);
}

TEST(node_status_cadence, immediate_event_resets_deadline_and_continuous_writes_do_not_postpone_it)
{
    using namespace std::chrono_literals;
    boost::asio::io_context                            executor;
    node_status_registry_s                             registry(executor, 60ms);
    const node_status_handle_s                         node("node");
    boost::asio::steady_timer                          event(executor);
    boost::asio::steady_timer                          writer(executor);
    std::vector<std::chrono::steady_clock::time_point> sent;
    std::vector<nlohmann::json>                        values;
    registry.set_callback([&](const auto& batch) {
        sent.push_back(std::chrono::steady_clock::now());
        values.push_back(batch.front().status);
    });
    registry.write(node, status::connected_status_s{.connected = false});
    registry.publish();
    executor.poll();
    event.expires_after(25ms);
    event.async_wait([&](const auto& error) {
        ASSERT_FALSE(error);
        registry.write(node, status::connected_status_s{.connected = true}, status_delivery_e::immediate);
        registry.publish();
    });
    uint64_t                                              frame = 0;
    std::function<void(const boost::system::error_code&)> tick;
    tick = [&](const auto& error) {
        ASSERT_FALSE(error);
        registry.write(node, status::ndi_output_metrics_status_s{.frames_sent = ++frame});
        registry.publish();
        if (frame < 10) {
            writer.expires_after(5ms);
            writer.async_wait(tick);
        }
    };
    writer.expires_after(30ms);
    writer.async_wait(tick);
    executor.restart();
    executor.run();
    ASSERT_GE(sent.size(), 3);
    EXPECT_TRUE(values[1].at("connected").get<bool>());
    EXPECT_GE(sent[2] - sent[1], 60ms);
    EXPECT_GE(sent[2], sent[0] + 85ms);
    EXPECT_EQ(values.back().at("frames_sent"), 10);
    // With no new writes, no recurring heartbeat or outstanding timer remains.
    executor.restart();
    EXPECT_EQ(executor.poll(), 0);
}

TEST(node_status_cadence, removal_and_shutdown_cancel_delayed_publication)
{
    using namespace std::chrono_literals;
    boost::asio::io_context    executor;
    node_status_registry_s     registry(executor, 1h);
    const node_status_handle_s node("node");
    size_t                     callbacks = 0;
    registry.set_callback([&](const auto&) { ++callbacks; });
    registry.write(node, status::connected_status_s{.connected = false});
    registry.publish();
    executor.poll();
    registry.write(node, status::connected_status_s{.connected = true});
    registry.publish();
    executor.restart();
    executor.poll();
    registry.remove_node(node);
    executor.run(); // Would hang for an hour if removal didn't cancel its timer.
    EXPECT_EQ(callbacks, 1);
    const node_status_handle_s replacement("node");
    registry.write(replacement, status::connected_status_s{.connected = true});
    registry.publish();
    executor.restart();
    executor.poll();
    registry.write(replacement, status::connected_status_s{.connected = false});
    registry.publish();
    executor.restart();
    executor.poll();
    registry.stop();
    executor.run();
    EXPECT_EQ(callbacks, 2);
}

TEST(node_status_cadence, invalid_report_interval_is_rejected)
{
    boost::asio::io_context executor;
    EXPECT_THROW(node_status_registry_s(executor, std::chrono::seconds(0)), std::invalid_argument);
}

} // namespace miximus::core::tests
