#include "core/node_actions.hpp"
#include "nodes/node.hpp"

#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace miximus::core { namespace {
using nodes::action_result_s;
class action_node_s final : public nodes::node_i
{
  public:
    std::vector<nlohmann::json> received;
    std::thread::id             thread;
    std::string_view            type() const final { return "action_test"; }
    void
    execute(app_state_s* /* app */, const nodes::node_map_t& /* nodes */, const nodes::node_state_s& /* state */) final
    {
    }
    nodes::option_result_e normalize_option(std::string_view /* name */, nlohmann::json* /* value */) const final
    {
        return nodes::option_result_e::invalid;
    }
    action_result_s handle_action(app_state_s* /* app */,
                                  const nodes::node_state_s& state,
                                  std::string_view           name,
                                  const nlohmann::json&      payload) final
    {
        thread = std::this_thread::get_id();
        if (name == "throw") {
            throw std::runtime_error("injected failure");
        }
        if (name == "large") {
            return {.data = std::string(node_action_limits::MAX_PAYLOAD_BYTES, 'x')};
        }
        if (name != "echo") {
            return {.error = error_e::unsupported_action};
        }
        received.push_back(payload);
        return {
            .data = {{"payload", payload}, {"options", state.options}}
        };
    }
};

TEST(node_actions, OwnsPayloadAndDispatchesInOrderOnConsumerThreadWithSnapshotState)
{
    auto                         node = std::make_shared<action_node_s>();
    std::vector<action_result_s> replies;
    node_actions_s               queue;
    const auto                   reply = [&](action_result_s result) { replies.push_back(std::move(result)); };
    std::thread                  producer([&] {
        nlohmann::json payload = {
            {"nested", {1, 2, 3}}
        };
        EXPECT_EQ(queue.enqueue("node", node, "echo", payload, reply), error_e::no_error);
        payload["nested"] = false;
        EXPECT_EQ(queue.enqueue("node", node, "echo", nullptr, reply), error_e::no_error);
    });
    producer.join();
    EXPECT_TRUE(replies.empty());
    EXPECT_TRUE(node->received.empty());
    const nodes::node_map_t snapshot{
        {"node", {.node = node, .state = {.con_map = {}, .options = {{"current", true}}}}}
    };
    node_actions_s::dispatch(queue.take_batch(), nullptr, snapshot);
    ASSERT_EQ(replies.size(), 2);
    EXPECT_EQ(node->thread, std::this_thread::get_id());
    EXPECT_EQ(replies[0].data["payload"],
              nlohmann::json({
                  {"nested", {1, 2, 3}}
    }));
    EXPECT_TRUE(replies[0].data["options"]["current"]);
    EXPECT_TRUE(replies[1].data["payload"].is_null());
}

TEST(node_actions, RemovedOrReplacedNodeNeverReceivesOldAction)
{
    auto                         old         = std::make_shared<action_node_s>();
    auto                         replacement = std::make_shared<action_node_s>();
    std::vector<action_result_s> replies;
    node_actions_s               queue;
    const auto                   reply = [&](action_result_s result) { replies.push_back(std::move(result)); };
    ASSERT_EQ(queue.enqueue("same-id", old, "echo", 7, reply), error_e::no_error);
    ASSERT_EQ(queue.enqueue("removed", old, "echo", 8, reply), error_e::no_error);
    const nodes::node_map_t snapshot{
        {"same-id", {.node = replacement, .state = {}}}
    };
    node_actions_s::dispatch(queue.take_batch(), nullptr, snapshot);
    ASSERT_EQ(replies.size(), 2);
    EXPECT_EQ(replies[0].error, error_e::not_found);
    EXPECT_EQ(replies[1].error, error_e::not_found);
    EXPECT_TRUE(old->received.empty());
    EXPECT_TRUE(replacement->received.empty());
}

TEST(node_actions, PendingRequestsDoNotRetainRemovedNodes)
{
    auto                               node     = std::make_shared<action_node_s>();
    const std::weak_ptr<nodes::node_i> lifetime = node;
    std::vector<error_e>               replies;
    node_actions_s                     queue;
    ASSERT_EQ(queue.enqueue(
                  "removed", node, "echo", {}, [&](const action_result_s& result) { replies.push_back(result.error); }),
              error_e::no_error);
    node.reset();
    EXPECT_TRUE(lifetime.expired());
    node_actions_s::dispatch(queue.take_batch(), nullptr, {});
    EXPECT_EQ(replies, (std::vector{error_e::not_found}));
}

TEST(node_actions, EnforcesAdmissionAndFrameLimitsWithoutCallingRejectedReply)
{
    auto           node = std::make_shared<action_node_s>();
    size_t         replies{};
    node_actions_s queue;
    const auto     reply = [&](const action_result_s& /* result */) { ++replies; };
    EXPECT_EQ(queue.enqueue("n", node, "", {}, reply), error_e::invalid_payload);
    EXPECT_EQ(queue.enqueue("n", node, "echo", std::string(node_action_limits::MAX_PAYLOAD_BYTES, 'x'), reply),
              error_e::invalid_payload);
    nlohmann::json deep = nullptr;
    for (size_t depth = 0; depth <= node_action_limits::MAX_JSON_DEPTH; ++depth) {
        deep = nlohmann::json::array({std::move(deep)});
    }
    EXPECT_EQ(queue.enqueue("n", node, "echo", deep, reply), error_e::invalid_payload);
    const auto wide = nlohmann::json(std::vector<int>(node_action_limits::MAX_JSON_VALUES, 0));
    EXPECT_EQ(queue.enqueue("n", node, "echo", wide, reply), error_e::invalid_payload);
    for (size_t i = 0; i < node_actions_s::MAX_PENDING; ++i) {
        const auto id = std::to_string(i / node_actions_s::MAX_PER_NODE);
        ASSERT_EQ(queue.enqueue(id, node, "echo", i, reply), error_e::no_error);
        if (i == node_actions_s::MAX_PER_NODE - 1) {
            EXPECT_EQ(queue.enqueue(id, node, "echo", 0, reply), error_e::busy);
        }
    }
    EXPECT_EQ(queue.enqueue("extra", node, "echo", 0, reply), error_e::busy);
    EXPECT_EQ(replies, 0);
    {
        auto batch = queue.take_batch();
        EXPECT_EQ(batch.size(), node_actions_s::MAX_PER_FRAME);
        // Abandoning a frame must still settle its accepted actions.
    }
    EXPECT_EQ(replies, node_actions_s::MAX_PER_FRAME);
    queue.close();
    EXPECT_EQ(replies, node_actions_s::MAX_PENDING);
    queue.close();
    EXPECT_EQ(replies, node_actions_s::MAX_PENDING);
    EXPECT_EQ(queue.enqueue("n", node, "echo", {}, reply), error_e::cancelled);
}

TEST(node_actions, ExpiresWithoutExecutionAndCancelsOnShutdownOutsideLocks)
{
    auto                 node = std::make_shared<action_node_s>();
    std::vector<error_e> replies;
    node_actions_s       queue;
    const auto           reply = [&](const action_result_s& result) {
        replies.push_back(result.error);
        EXPECT_EQ(queue.take_batch().size(), 0); // Reentrant responder cannot deadlock.
    };
    const auto now = node_actions_s::clock_t::now();
    ASSERT_EQ(queue.enqueue("n", node, "echo", {}, reply, now), error_e::no_error);
    node_actions_s::dispatch(queue.take_batch(),
                             nullptr,
                             {
                                 {"n", {.node = node, .state = {}}}
    },
                             now + node_actions_s::MAX_AGE);
    ASSERT_EQ(queue.enqueue("n", node, "echo", {}, reply), error_e::no_error);
    queue.close();
    EXPECT_EQ(replies, (std::vector{error_e::expired, error_e::cancelled}));
    EXPECT_TRUE(node->received.empty());
}

TEST(node_actions, HandlerAndResponderFailuresDoNotPreventLaterReplies)
{
    auto                 node = std::make_shared<action_node_s>();
    std::vector<error_e> replies;
    node_actions_s       queue;
    const auto           reply = [&](const action_result_s& result) { replies.push_back(result.error); };
    ASSERT_EQ(queue.enqueue("n", node, "throw", {}, reply), error_e::no_error);
    ASSERT_EQ(queue.enqueue("n", node, "unknown", {}, reply), error_e::no_error);
    ASSERT_EQ(queue.enqueue("n", node, "large", {}, reply), error_e::no_error);
    ASSERT_EQ(queue.enqueue("n",
                            node,
                            "echo",
                            {},
                            [](const action_result_s& /* result */) { throw std::runtime_error("disconnected"); }),
              error_e::no_error);
    ASSERT_EQ(queue.enqueue("n", node, "echo", {}, reply), error_e::no_error);
    node_actions_s::dispatch(queue.take_batch(),
                             nullptr,
                             {
                                 {"n", {.node = node, .state = {}}}
    });
    EXPECT_EQ(replies,
              (std::vector{
                  error_e::internal_error, error_e::unsupported_action, error_e::internal_error, error_e::no_error}));
}
}} // namespace miximus::core
