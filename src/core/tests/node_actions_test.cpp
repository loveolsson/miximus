#include "core/configuration.hpp"
#include "core/node_manager.hpp"
#include "logger/logger.hpp"
#include "nodes/action.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "types/cef_status.hpp"
#include "types/node_action_contracts.hpp"
#include "utils/failure_shutdown.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace miximus::core {
// Exercise the actual manager admission and frame handoff without constructing a
// GPU/application. These helpers run only on quiescent test-owned graphs.
struct node_manager_test_access_s
{
    static void define(node_manager_s& manager, nodes::constructor_t factory)
    {
        manager.node_definitions_.emplace("action_test", std::move(factory));
    }
    static node_actions_s::batch_t  take(node_manager_s& manager) { return manager.take_frame_updates(); }
    static const nodes::node_map_t& snapshot(node_manager_s& manager) { return manager.nodes_copy_; }
    static const nodes::node_map_t& config(node_manager_s& manager) { return manager.nodes_; }
    static void                     close(node_manager_s& manager) { manager.close_actions(); }
};
namespace {
using nodes::action_dispatch_e;
using nodes::action_result_s;
using nodes::action_s;
using access = node_manager_test_access_s;

struct observations_s
{
    std::vector<nlohmann::json>  frames;
    std::vector<std::thread::id> admissions;
    std::vector<std::thread::id> deliveries;
    std::vector<action_s>        deferred;
    size_t                       starts{};
};
class action_node_s final : public nodes::node_i
{
    std::shared_ptr<observations_s>   observed_;
    nodes::input_interface_s<double>  in_{*this, "in"};
    nodes::output_interface_s<double> out_{*this, "out"};

  public:
    explicit action_node_s(std::shared_ptr<observations_s> observed)
        : observed_(std::move(observed))
    {
    }
    std::string_view type() const final { return "action_test"; }
    void
    execute(app_state_s* /* app */, const nodes::node_map_t& /* nodes */, const nodes::node_state_s& /* state */) final
    {
    }
    nlohmann::json get_default_options() const final
    {
        return {
            {"value", 0}
        };
    }
    nodes::option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name != "value" || !value->is_number_integer() || value->get<int>() < 0) {
            return nodes::option_result_e::invalid;
        }
        if (value->get<int>() > 100) {
            *value = 100;
            return nodes::option_result_e::corrected;
        }
        return nodes::option_result_e::ok;
    }
    action_dispatch_e handle_action(nodes::action_context_s& context, action_s& action) const final
    {
        observed_->admissions.push_back(std::this_thread::get_id());
        return handle_config(context, action);
    }
    action_dispatch_e handle_config(nodes::action_context_s& context, action_s& action) const
    {
        const auto& name   = action.name;
        auto        update = [&](nlohmann::json value) {
            const auto result = context.update_settings({
                {"value", std::move(value)}
            });
            if (result.error != error_e::no_error) {
                action.fail(result.error);
                return false;
            }
            return true;
        };
        if (name == "throw_admission") {
            throw std::runtime_error("injected admission failure");
        }
        if (name == "increment") {
            if (update(context.state().options.at("value").get<int>() + 1)) {
                action.complete();
            }
            return action_dispatch_e::handled;
        }
        if (name == "invalid_patch") {
            update(-1);
            return action_dispatch_e::handled;
        }
        if (name == "reject_patch") {
            update(99);
            action.fail(error_e::unavailable);
            return action_dispatch_e::handled;
        }
        if (name == "patch_frame") {
            return update(action.payload) ? action_dispatch_e::frame : action_dispatch_e::handled;
        }
        if (name == "first_input") {
            const auto& connections = context.state().get_connection_set("in");
            const auto* source      = connections.empty() ? nullptr : context.find_state(connections.front().from_node);
            if (source == nullptr) {
                action.fail(error_e::not_found);
            } else if (update(source->options.at("value"))) {
                action.complete();
            }
            return action_dispatch_e::handled;
        }
        if (name == "captured") {
            action.payload = context.state().options.at("value");
            return action_dispatch_e::frame;
        }
        if (name == "independent" || name == "async" || name == "throw_start") {
            context.defer([observed = observed_](app_state_s* /* app */, action_s owned) {
                ++observed->starts;
                if (owned.name == "throw_start") {
                    throw std::runtime_error("injected start failure");
                }
                if (owned.name == "async") {
                    observed->deferred.push_back(std::move(owned));
                } else {
                    owned.complete();
                }
            });
            return action_dispatch_e::handled;
        }
        if (name == "large") {
            if (update(99)) {
                action.complete({.data = std::string(size_t{128} * 1024, 'x')});
            }
            return action_dispatch_e::handled;
        }
        if (name == "echo" || name == "ignore" || name == "throw_frame" || name == "async_frame" ||
            name == "large_frame" || name == "unhandled") {
            return action_dispatch_e::frame;
        }
        return action_dispatch_e::unhandled;
    }
    action_dispatch_e
    handle_frame_action(app_state_s* /* app */, const nodes::node_state_s& state, action_s& action) final
    {
        if (action.name == "unhandled") {
            return action_dispatch_e::unhandled;
        }
        handle_frame(state, action);
        return action_dispatch_e::handled;
    }
    void handle_frame(const nodes::node_state_s& state, action_s& action)
    {
        observed_->deliveries.push_back(std::this_thread::get_id());
        if (action.name == "ignore") {
            return;
        }
        if (action.name == "throw_frame") {
            throw std::runtime_error("injected frame failure");
        }
        if (action.name == "async_frame") {
            observed_->deferred.push_back(std::move(action));
            return;
        }
        if (action.name == "large_frame") {
            action.complete({.data = std::string(size_t{128} * 1024, 'x')});
            return;
        }
        observed_->frames.push_back({
            {"payload", action.payload           },
            {"value",   state.options.at("value")}
        });
        action.complete({.data = observed_->frames.back()});
    }
};

struct update_s
{
    std::string                  id;
    nlohmann::json               options;
    bool                         corrected{};
    std::optional<origin_info_s> origin;
};
class recording_adapter_s final : public node_manager_s::adapter_i
{
    std::reference_wrapper<std::vector<update_s>> updates_;

  public:
    explicit recording_adapter_s(std::vector<update_s>& updates)
        : updates_(updates)
    {
    }
    void emit_add_node(std::string_view /* type */,
                       std::string_view /* id */,
                       const nlohmann::json& /* options */,
                       const std::optional<origin_info_s>& /* origin */) final
    {
    }
    void emit_remove_node(std::string_view /* id */, const std::optional<origin_info_s>& /* origin */) final {}
    void emit_update_node(std::string_view                    id,
                          const nlohmann::json&               options,
                          bool                                corrected,
                          const std::optional<origin_info_s>& origin) final
    {
        updates_.get().push_back({.id = std::string(id), .options = options, .corrected = corrected, .origin = origin});
    }
    void emit_add_connection(const connection_s& /* con */, const std::optional<origin_info_s>& /* origin */) final {}
    void emit_remove_connection(const connection_s& /* con */, const std::optional<origin_info_s>& /* origin */) final
    {
    }
};

struct action_manager_test_s : testing::Test
{
    std::shared_ptr<observations_s> observed = std::make_shared<observations_s>();
    std::vector<action_result_s>    replies;
    std::unique_ptr<node_manager_s> manager;
    void                            SetUp() override
    {
        if (!getlog("app")) {
            logger::init_loggers(spdlog::level::off);
        }
        manager = std::make_unique<node_manager_s>();
        access::define(*manager, [state = observed] { return std::make_shared<action_node_s>(state); });
        ASSERT_EQ(manager->handle_add_node("action_test", "n", nlohmann::json::object()), error_e::no_error);
    }
    node_manager_s::action_request_s request(std::string name, nlohmann::json payload = nullptr, std::string id = "n")
    {
        return {.id      = std::move(id),
                .name    = std::move(name),
                .payload = std::move(payload),
                .reply   = [this](action_result_s result) { replies.push_back(std::move(result)); }};
    }
    void send(std::string name, nlohmann::json payload = nullptr, std::string id = "n")
    {
        auto action = request(std::move(name), std::move(payload), std::move(id));
        EXPECT_EQ(manager->handle_node_action(nullptr, action.id, action.name, action.payload, std::move(action.reply)),
                  error_e::no_error);
    }
    void frame() const
    {
        auto actions = access::take(*manager);
        actions.dispatch(nullptr, access::snapshot(*manager));
    }
    int value(std::string_view id = "n") const
    {
        return access::config(*manager).at(std::string(id)).state.options.at("value").get<int>();
    }
};

TEST_F(action_manager_test_s, RecoverySnapshotTracksAcceptedGraphChanges)
{
    configuration_s configuration(*manager);
    configuration.enable_recovery("settings.json");
    const auto initial = utils::recovery_settings();
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(nlohmann::json::parse(initial->contents), configuration.get_config());
    ASSERT_EQ(manager
                  ->handle_update_node("n",
                                       {
                                           {"value", 12}
    })
                  .error,
              error_e::no_error);
    EXPECT_EQ(nlohmann::json::parse(utils::recovery_settings()->contents), configuration.get_config());
    EXPECT_NE(utils::recovery_settings()->contents, initial->contents);
    ASSERT_EQ(manager->handle_add_node("action_test", "other", nlohmann::json::object()), error_e::no_error);
    EXPECT_EQ(nlohmann::json::parse(utils::recovery_settings()->contents), configuration.get_config());
    const connection_s connection{.from_node = "n", .from_interface = "out", .to_node = "other", .to_interface = "in"};
    ASSERT_EQ(manager->handle_add_connection(connection), error_e::no_error);
    EXPECT_EQ(nlohmann::json::parse(utils::recovery_settings()->contents), configuration.get_config());
    ASSERT_EQ(manager->handle_remove_node("other"), error_e::no_error);
    EXPECT_EQ(nlohmann::json::parse(utils::recovery_settings()->contents), configuration.get_config());
    const auto accepted = utils::recovery_settings();
    EXPECT_NE(manager
                  ->handle_update_node("n",
                                       {
                                           {"value", -1}
    })
                  .error,
              error_e::no_error);
    EXPECT_EQ(utils::recovery_settings(), accepted);
}

TEST_F(action_manager_test_s, ConfigAdmissionAndFrameDeliveryUseTheirOwnThreadsAndOwnedPayload)
{
    const auto producer_id = [&] {
        std::thread::id id;
        auto            producer = std::async(std::launch::async, [&] {
            id                     = std::this_thread::get_id();
            nlohmann::json payload = {
                {"nested", {1, 2, 3}}
            };
            send("echo", payload);
            payload["nested"] = false;
        });
        producer.get();
        return id;
    }();
    EXPECT_TRUE(replies.empty());
    EXPECT_TRUE(observed->frames.empty());
    EXPECT_EQ(observed->admissions, std::vector{producer_id});
    EXPECT_EQ(manager
                  ->handle_update_node("n",
                                       {
                                           {"value", 4}
    })
                  .error,
              error_e::no_error);
    frame();
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].data.at("payload"),
              (nlohmann::json{
                  {"nested", {1, 2, 3}}
    }));
    EXPECT_EQ(replies[0].data.at("value"), 4);
    EXPECT_EQ(observed->deliveries, std::vector{std::this_thread::get_id()});
    frame();
    EXPECT_EQ(replies.size(), 1); // Transient actions are never replayed.
}

TEST_F(action_manager_test_s, RejectsAllExplicitSettingsBeforeInvokingAnyAction)
{
    ASSERT_EQ(manager->handle_add_node("action_test", "other", nlohmann::json::object()), error_e::no_error);
    const std::vector<node_manager_s::option_update_s> updates{
        {.id = "n",     .options = {{"value", 7}} },
        {.id = "other", .options = {{"value", -1}}}
    };
    EXPECT_EQ(manager->handle_control_batch(nullptr, updates, {request("increment")}).error, error_e::invalid_options);
    EXPECT_EQ(value(), 0);
    EXPECT_EQ(value("other"), 0);
    EXPECT_TRUE(observed->admissions.empty());
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::invalid_options);
}

TEST_F(action_manager_test_s, ActionsMutateEvolvingConfigAndFailIndependently)
{
    const std::vector<node_manager_s::option_update_s> updates{
        {.id = "n", .options = {{"value", 8}}}
    };
    auto result = manager->handle_control_batch(nullptr,
                                                updates,
                                                {request("increment"),
                                                 request("invalid_patch"),
                                                 request("throw_admission"),
                                                 request("reject_patch"),
                                                 request("increment"),
                                                 request("captured"),
                                                 request("patch_frame", 200)});
    EXPECT_EQ(result.error, error_e::no_error);
    EXPECT_EQ(value(), 100);
    ASSERT_EQ(replies.size(), 5);
    EXPECT_EQ(replies[0].error, error_e::no_error);
    EXPECT_EQ(replies[1].error, error_e::invalid_options);
    EXPECT_EQ(replies[2].error, error_e::internal_error);
    EXPECT_EQ(replies[3].error, error_e::unavailable);
    EXPECT_EQ(replies[4].error, error_e::no_error);
    frame();
    ASSERT_EQ(replies.size(), 7);
    EXPECT_EQ(replies[5].data.at("payload"), 10); // Admission-time argument.
    EXPECT_EQ(replies[5].data.at("value"), 100);  // Final frame configuration.
    EXPECT_EQ(replies[6].data.at("value"), 100);
}

TEST_F(action_manager_test_s, CanDeriveSettingsFromAuthoritativeConnectionsAndOtherNodes)
{
    ASSERT_EQ(manager->handle_add_node("action_test",
                                       "source",
                                       {
                                           {"value", 17}
    }),
              error_e::no_error);
    ASSERT_EQ(manager->handle_add_connection(
                  {.from_node = "source", .from_interface = "out", .to_node = "n", .to_interface = "in"}),
              error_e::no_error);
    const std::vector<node_manager_s::option_update_s> updates{
        {.id = "source", .options = {{"value", 42}}}
    };
    EXPECT_EQ(manager->handle_control_batch(nullptr, updates, {request("first_input")}).error, error_e::no_error);
    EXPECT_EQ(value(), 42);
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::no_error);
}

TEST_F(action_manager_test_s, ConfigWorkAndDeferredRepliesDoNotRequireAFrame)
{
    send("independent");
    send("async");
    EXPECT_EQ(observed->starts, 2);
    ASSERT_EQ(replies.size(), 1);
    ASSERT_EQ(observed->deferred.size(), 1);
    auto completion = std::move(observed->deferred.back());
    observed->deferred.clear();
    std::thread worker([completion = std::move(completion)]() mutable { completion.complete({.data = 7}); });
    worker.join();
    ASSERT_EQ(replies.size(), 2);
    EXPECT_EQ(replies.back().data, 7);
    EXPECT_TRUE(observed->frames.empty());
}

TEST_F(action_manager_test_s, FrameConsumptionCanTransferCompletionToAsyncWork)
{
    send("async_frame",
         {
             {"marker", 7}
    });
    frame();
    EXPECT_TRUE(replies.empty());
    ASSERT_EQ(observed->deferred.size(), 1);
    EXPECT_EQ(observed->deferred.front().name, "async_frame");
    EXPECT_EQ(observed->deferred.front().payload.at("marker"), 7);
    EXPECT_EQ(observed->deferred.front().target, access::config(*manager).at("n").node->handle());
    observed->deferred.front().complete({.data = "done"});
    observed->deferred.front().complete(); // Already settled, cannot reply twice.
    observed->deferred.clear();
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].data, "done");
}

TEST_F(action_manager_test_s, UnhandledFrameActionFailsInSharedDispatcher)
{
    send("unhandled");
    send("echo");
    frame();
    ASSERT_EQ(replies.size(), 2);
    EXPECT_EQ(replies[0].error, error_e::internal_error);
    EXPECT_EQ(replies[0].message, "Node did not handle frame action");
    EXPECT_EQ(replies[1].error, error_e::no_error);
}

TEST_F(action_manager_test_s, AbandonedAsyncCompletionsAndIgnoredFrameActionsFail)
{
    send("async");
    observed->deferred.clear();
    send("ignore");
    send("throw_frame");
    send("throw_start");
    send("echo");
    frame();
    ASSERT_EQ(replies.size(), 5);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(replies[i].error, error_e::internal_error);
    }
    EXPECT_EQ(replies[4].error, error_e::no_error);
}

TEST_F(action_manager_test_s, Enforces64PerInstanceAndDoesNotApplyRejectedActionSettings)
{
    for (size_t i = 0; i < 64; ++i) {
        send("echo", i);
    }
    send("patch_frame", 99);
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::busy);
    EXPECT_EQ(value(), 0);
    auto batch = access::take(*manager);
    EXPECT_EQ(batch.size(), 64);
    for (size_t i = 0; i < 64; ++i) {
        send("echo", i);
    }
    send("echo");
    EXPECT_EQ(replies.size(), 2);
    batch.dispatch(nullptr, access::snapshot(*manager));
    frame();
    EXPECT_EQ(replies.size(), 130);
}

TEST_F(action_manager_test_s, DeliversHundredsOfNodesInOneFrame)
{
    for (size_t i = 0; i < 500; ++i) {
        const auto id = std::to_string(i);
        ASSERT_EQ(manager->handle_add_node("action_test", id, nlohmann::json::object()), error_e::no_error);
        send("echo", i, id);
    }
    auto batch = access::take(*manager);
    EXPECT_EQ(batch.size(), 500);
    batch.dispatch(nullptr, access::snapshot(*manager));
    ASSERT_EQ(replies.size(), 500);
    for (size_t i = 0; i < replies.size(); ++i) {
        EXPECT_EQ(replies[i].error, error_e::no_error);
        EXPECT_EQ(replies[i].data.at("payload"), i);
    }
    EXPECT_EQ(observed->frames.size(), 500);
}

TEST_F(action_manager_test_s, RemovalCancelsPendingWorkAndReplacementHasFreshCapacity)
{
    const auto                         old      = access::config(*manager).at("n").node->handle();
    const std::weak_ptr<nodes::node_i> lifetime = access::config(*manager).at("n").node;
    for (size_t i = 0; i < 64; ++i) {
        send("echo");
    }
    ASSERT_EQ(manager->handle_remove_node("n"), error_e::no_error);
    EXPECT_FALSE(old.active());
    EXPECT_TRUE(lifetime.expired());
    ASSERT_EQ(replies.size(), 64);
    for (const auto& reply : replies) {
        EXPECT_EQ(reply.error, error_e::not_found);
    }
    ASSERT_EQ(manager->handle_add_node("action_test", "n", nlohmann::json::object()), error_e::no_error);
    EXPECT_NE(old, access::config(*manager).at("n").node->handle());
    send("echo");
    frame();
    ASSERT_EQ(replies.size(), 65);
    EXPECT_EQ(replies.back().error, error_e::no_error);
    EXPECT_EQ(observed->frames.size(), 1);
}

TEST_F(action_manager_test_s, ChangesAfterBoundaryDoNotInvalidateAlreadySelectedFrame)
{
    send("echo");
    auto batch = access::take(*manager);
    ASSERT_EQ(manager->handle_remove_node("n"), error_e::no_error);
    ASSERT_EQ(manager->handle_add_node("action_test",
                                       "n",
                                       {
                                           {"value", 7}
    }),
              error_e::no_error);
    batch.dispatch(nullptr, access::snapshot(*manager));
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::no_error);
    EXPECT_EQ(replies[0].data.at("value"), 0);
    send("echo");
    frame();
    EXPECT_EQ(replies.back().data.at("value"), 7);
}

TEST_F(action_manager_test_s, IdentityRejectsDeliveryToReplacementSnapshot)
{
    send("echo");
    auto batch = access::take(*manager);
    ASSERT_EQ(manager->handle_remove_node("n"), error_e::no_error);
    ASSERT_EQ(manager->handle_add_node("action_test", "n", nlohmann::json::object()), error_e::no_error);
    auto next = access::take(*manager);
    batch.dispatch(nullptr, access::snapshot(*manager));
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::not_found);
    EXPECT_TRUE(observed->frames.empty());
}

TEST_F(action_manager_test_s, ExpiryAbandonedFramesAndShutdownSettleRequestsOutsideLocks)
{
    const auto now = node_actions_s::clock_t::now();
    EXPECT_EQ(manager->handle_control_batch(nullptr, {}, {request("echo")}, {}, now).error, error_e::no_error);
    auto batch = access::take(*manager);
    batch.dispatch(nullptr, access::snapshot(*manager), now + node_actions_s::MAX_AGE);
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::expired);
    send("echo");
    {
        auto abandoned = access::take(*manager);
    }
    EXPECT_EQ(replies.back().error, error_e::cancelled);
    auto reentrant  = request("echo");
    reentrant.reply = [this](action_result_s result) {
        EXPECT_EQ(result.error, error_e::cancelled);
        EXPECT_EQ(manager
                      ->handle_update_node("n",
                                           {
                                               {"value", 2}
        })
                      .error,
                  error_e::cancelled);
        replies.push_back(std::move(result));
    };
    EXPECT_EQ(manager->handle_control_batch(nullptr, {}, {std::move(reentrant)}).error, error_e::no_error);
    const auto handle = access::config(*manager).at("n").node->handle();
    access::close(*manager);
    EXPECT_FALSE(handle.active());
    EXPECT_EQ(replies.size(), 3);
    access::close(*manager);
    EXPECT_EQ(replies.size(), 3);
}

TEST_F(action_manager_test_s, AcceptsLargeRequestsAndResultsAndIsolatesResponderExceptions)
{
    auto reply = [this](action_result_s result) { replies.push_back(std::move(result)); };
    EXPECT_EQ(manager->handle_node_action(nullptr, "n", "", {}, reply), error_e::invalid_payload);
    EXPECT_TRUE(replies.empty());
    const std::string large(size_t{128} * 1024, 'x');
    send("large");
    EXPECT_EQ(value(), 99);
    send("large_frame");
    send("echo", large);
    EXPECT_EQ(manager->handle_node_action(
                  nullptr, "n", "echo", {}, [](const action_result_s&) { throw std::runtime_error("reply failed"); }),
              error_e::no_error);
    send("echo");
    frame();
    ASSERT_EQ(replies.size(), 4);
    for (const auto& result : replies) {
        EXPECT_EQ(result.error, error_e::no_error);
    }
    EXPECT_EQ(replies[0].data, large);
    EXPECT_EQ(replies[1].data, large);
    EXPECT_EQ(replies[2].data.at("payload"), large);
}

TEST_F(action_manager_test_s, ImmediateReplyCanReenterConfigurationAfterCommit)
{
    EXPECT_EQ(manager->handle_node_action(nullptr,
                                          "n",
                                          "increment",
                                          {},
                                          [this](const action_result_s& result) {
                                              EXPECT_EQ(result.error, error_e::no_error);
                                              EXPECT_EQ(value(), 1);
                                              EXPECT_EQ(manager
                                                            ->handle_update_node("n",
                                                                                 {
                                                                                     {"value", 9}
                                              })
                                                            .error,
                                                        error_e::no_error);
                                          }),
              error_e::no_error);
    EXPECT_EQ(value(), 9);
}
TEST_F(action_manager_test_s, ActionSettingsBroadcastToOriginAndPersistWithoutTransientActions)
{
    std::vector<update_s> updates;
    manager->add_adapter(std::make_unique<recording_adapter_s>(updates));
    const origin_info_s                                origin{.id = 17, .token = "request"};
    const std::vector<node_manager_s::option_update_s> options{
        {.id = "n", .options = {{"value", 7}}}
    };
    EXPECT_EQ(manager->handle_control_batch(nullptr, options, {request("patch_frame", 101)}, origin).error,
              error_e::no_error);
    ASSERT_EQ(updates.size(), 1);
    EXPECT_EQ(updates[0].options.at("value"), 100);
    EXPECT_TRUE(updates[0].corrected);
    EXPECT_FALSE(updates[0].origin.has_value()); // The requesting editor must receive the derived value.
    configuration_s configuration(*manager);
    const auto      node = configuration.get_node("n");
    if (!node.has_value()) {
        FAIL() << "Expected the persisted node";
        return;
    }
    EXPECT_EQ(node->at("options").at("value"), 100);
    EXPECT_FALSE(node->contains("actions"));
    EXPECT_FALSE(node->at("options").contains("actions"));
    frame();
    EXPECT_EQ(manager
                  ->handle_update_node("n",
                                       {
                                           {"value", 2}
    },
                                       origin)
                  .error,
              error_e::no_error);
    ASSERT_EQ(updates.size(), 2);
    const auto& update_origin = updates.back().origin;
    if (!update_origin.has_value()) {
        FAIL() << "Expected the explicit update origin";
        return;
    }
    EXPECT_EQ(update_origin->id, 17);
    manager->clear_adapters();
}

TEST_F(action_manager_test_s, SettingsAndActionsAcrossNodesShareOneSnapshotAndLaterWorkWaits)
{
    ASSERT_EQ(manager->handle_add_node("action_test", "other", nlohmann::json::object()), error_e::no_error);
    const std::vector<node_manager_s::option_update_s> updates{
        {.id = "n",     .options = {{"value", 7}}},
        {.id = "other", .options = {{"value", 8}}}
    };
    EXPECT_EQ(
        manager->handle_control_batch(nullptr, updates, {request("echo"), request("echo", nullptr, "other")}).error,
        error_e::no_error);
    auto batch = access::take(*manager);
    send("patch_frame", 20);
    batch.dispatch(nullptr, access::snapshot(*manager));
    ASSERT_EQ(replies.size(), 2);
    EXPECT_EQ(replies[0].data.at("value"), 7);
    EXPECT_EQ(replies[1].data.at("value"), 8);
    frame();
    ASSERT_EQ(replies.size(), 3);
    EXPECT_EQ(replies.back().data.at("value"), 20);
}

TEST_F(action_manager_test_s, LateAsyncReplyDoesNotRetainOrMutateRemovedNode)
{
    send("async");
    const std::weak_ptr<nodes::node_i> old = access::config(*manager).at("n").node;
    EXPECT_EQ(manager->handle_remove_node("n"), error_e::no_error);
    EXPECT_TRUE(old.expired());
    ASSERT_EQ(manager->handle_add_node("action_test",
                                       "n",
                                       {
                                           {"value", 12}
    }),
              error_e::no_error);
    observed->deferred.front().complete({.data = "old request finished"});
    observed->deferred.clear();
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].data, "old request finished");
    EXPECT_EQ(value(), 12);
}

TEST_F(action_manager_test_s, DelayedConfigOperationsCannotTargetAReplacement)
{
    const auto old = access::config(*manager).at("n").node->handle();
    EXPECT_EQ(manager->handle_remove_node("n"), error_e::no_error);
    ASSERT_EQ(manager->handle_add_node("action_test",
                                       "n",
                                       {
                                           {"value", 12}
    }),
              error_e::no_error);
    const std::vector<node_manager_s::option_update_s> updates{
        {.id = "n", .options = {{"value", 20}}, .target = old}
    };
    EXPECT_EQ(manager->handle_control_batch(nullptr, updates, {request("increment")}).error, error_e::not_found);
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::not_found);
    replies.clear();
    auto action   = request("increment");
    action.target = old;
    EXPECT_EQ(manager->handle_control_batch(nullptr, {}, {std::move(action)}).error, error_e::no_error);
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::not_found);
    EXPECT_EQ(value(), 12);
    auto current   = request("increment");
    current.target = access::config(*manager).at("n").node->handle();
    EXPECT_EQ(manager->handle_control_batch(nullptr, {}, {std::move(current)}).error, error_e::no_error);
    EXPECT_EQ(value(), 13);
}

TEST(node_actions, typed_contract_failures_are_decoding_errors)
{
    action_s action("browser",
                    "reload",
                    {
                        {"ignore_cache", "yes"}
    },
                    [](auto) {});
    EXPECT_FALSE(action.get_typed_payload<browser_reload_payload_s>());
    action.payload = {
        {"future_field", true}
    };
    const auto payload = action.get_typed_payload<browser_reload_payload_s>();
    ASSERT_TRUE(payload);
    EXPECT_FALSE(payload->ignore_cache);
    action.payload = "unknown-state";
    EXPECT_FALSE(action.get_typed_payload<cef_state_e>());
    action.payload = "ready";
    EXPECT_EQ(action.get_typed_payload<cef_state_e>(), cef_state_e::ready);
    action.complete();
}

TEST(NodeActionCompletion, PreservesLongMessagesWithoutSerializingResults)
{
    action_result_s   result;
    action_s          completion("n", "test", nullptr, [&](action_result_s value) { result = std::move(value); });
    const std::string message(2048, 'x');
    completion.complete({.error = error_e::unavailable, .message = message});
    EXPECT_EQ(result.error, error_e::unavailable);
    EXPECT_EQ(result.message, message);
}

TEST(NodeActionPlan, MissingFrameHandlerReportsAnImplementationError)
{
    class missing_handler_s final : public nodes::node_i
    {
      public:
        std::string_view type() const final { return "action_test"; }
        void             execute(app_state_s* /* app */,
                                 const nodes::node_map_t& /* nodes */,
                                 const nodes::node_state_s& /* state */) final
        {
        }
        nodes::option_result_e normalize_option(std::string_view /* name */, nlohmann::json* /* value */) const final
        {
            return nodes::option_result_e::invalid;
        }
        action_dispatch_e handle_action(nodes::action_context_s& /* context */, action_s& /* action */) const final
        {
            return action_dispatch_e::frame;
        }
    };
    if (!getlog("app")) {
        logger::init_loggers(spdlog::level::off);
    }
    node_manager_s manager;
    access::define(manager, [] { return std::make_shared<missing_handler_s>(); });
    ASSERT_EQ(manager.handle_add_node("action_test", "n", nlohmann::json::object()), error_e::no_error);
    std::vector<action_result_s> replies;
    ASSERT_EQ(manager.handle_node_action(
                  nullptr, "n", "test", nullptr, [&](action_result_s result) { replies.push_back(std::move(result)); }),
              error_e::no_error);
    auto batch = access::take(manager);
    batch.dispatch(nullptr, access::snapshot(manager));
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::internal_error);
    EXPECT_EQ(replies[0].message, "Node did not handle frame action");
}

TEST(NodeAction, MovesTheWholeRequestAndSettlesExactlyOnce)
{
    std::vector<action_result_s> replies;
    {
        action_s original(
            "n", "test", {1, 2, 3}, [&](action_result_s result) { replies.push_back(std::move(result)); });
        original.target     = node_handle_s("n");
        const auto identity = original.target;
        auto       moved    = std::move(original);
        // Verify the documented moved-from no-op contract, not accidental reuse.
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        original.fail(error_e::internal_error);
        EXPECT_EQ(moved.id, "n");
        EXPECT_EQ(moved.name, "test");
        EXPECT_EQ(moved.target, identity);
        EXPECT_EQ(moved.get_typed_payload<std::vector<int>>(), (std::vector<int>{1, 2, 3}));
        EXPECT_FALSE(moved.get_typed_payload<int>());
        moved.complete({.data = "done"});
        moved.fail(error_e::cancelled);
        // A moved-from action deliberately retains read-only outcome observation.
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_EQ(original.result_error(), error_e::no_error);
    }
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].data, "done");
}

TEST(NodeAction, MoveAssignmentSettlesTheAbandonedDestination)
{
    std::vector<error_e> results;
    auto                 reply = [&](const action_result_s& result) { results.push_back(result.error); };
    {
        action_s first("n", "first", {}, reply);
        action_s second("n", "second", {}, reply);
        second = std::move(first);
        second.complete();
    }
    EXPECT_EQ(results, (std::vector{error_e::internal_error, error_e::no_error}));
}

TEST(NodeAction, TimerFollowsMovesAndLateCompletionCannotReplyAgain)
{
    boost::asio::io_context executor;
    std::vector<error_e>    results;
    action_s action("n", "test", {}, [&](const action_result_s& result) { results.push_back(result.error); });
    action.set_deadline(action_s::clock_t::now(), executor.get_executor());
    auto worker = std::move(action);
    executor.run();
    ASSERT_EQ(results, std::vector{error_e::expired});
    worker.complete();
    worker.fail(error_e::cancelled);
    EXPECT_EQ(results.size(), 1);
}

TEST(NodeAction, ConsumingAFrameDisarmsItsWaitingDeadline)
{
    boost::asio::io_context executor;
    std::vector<error_e>    results;
    action_s   action("n", "test", {}, [&](const action_result_s& result) { results.push_back(result.error); });
    const auto now = action_s::clock_t::now();
    action.set_deadline(now + std::chrono::seconds(5), executor.get_executor());
    ASSERT_TRUE(action.consume(now));
    executor.run();
    EXPECT_FALSE(action.expire(now + std::chrono::seconds(10)));
    EXPECT_TRUE(results.empty());
    action.complete();
    EXPECT_EQ(results, std::vector{error_e::no_error});
}

TEST(NodeAction, TimeoutAndWorkerCompletionRaceHasOneWinner)
{
    for (int i = 0; i < 100; ++i) {
        boost::asio::io_context executor;
        std::atomic<size_t>     replies{};
        action_s                action("n", "test", {}, [&](const action_result_s&) { ++replies; });
        action.set_deadline(action_s::clock_t::now(), executor.get_executor());
        auto timer = std::async(std::launch::async, [&] { executor.run(); });
        action.complete();
        timer.get();
        action.fail(error_e::cancelled);
        EXPECT_EQ(replies.load(), 1);
    }
}

TEST_F(action_manager_test_s, RejectedBatchAnswersEveryUnprocessedAction)
{
    std::vector<size_t>                           tokens;
    std::vector<node_manager_s::action_request_s> actions;
    for (size_t token = 0; token < 3; ++token) {
        auto entry  = request("echo");
        entry.reply = [&, token](const action_result_s& result) {
            EXPECT_EQ(result.error, error_e::invalid_options);
            tokens.push_back(token);
            EXPECT_EQ(manager
                          ->handle_update_node("n",
                                               {
                                                   {"value", 3}
            })
                          .error,
                      error_e::no_error);
        };
        actions.push_back(std::move(entry));
    }
    const std::vector<node_manager_s::option_update_s> updates{
        {.id = "n", .options = {{"value", -1}}}
    };
    EXPECT_EQ(manager->handle_control_batch(nullptr, updates, std::move(actions)).error, error_e::invalid_options);
    EXPECT_EQ(tokens, (std::vector<size_t>{0, 1, 2}));
    EXPECT_TRUE(observed->admissions.empty());
    frame();
    EXPECT_EQ(tokens.size(), 3);
}

TEST_F(action_manager_test_s, ClosedManagerAndUnhandledActionReplyOnce)
{
    send("unknown");
    ASSERT_EQ(replies.size(), 1);
    EXPECT_EQ(replies[0].error, error_e::unsupported_action);
    access::close(*manager);
    send("echo");
    ASSERT_EQ(replies.size(), 2);
    EXPECT_EQ(replies[1].error, error_e::cancelled);
}

} // namespace
} // namespace miximus::core
