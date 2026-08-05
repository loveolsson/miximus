#include "types/web_message.hpp"
#include "web_server/server.hpp"
#include "web_server/typed_server.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace miximus;

class server_mock_s final : public web_server::server_s
{
  public:
    web_server::callback_t subscription;
    std::optional<topic_e> subscribed_topic;
    int64_t                sent_connection_id{};
    nlohmann::json         sent_message_value;
    bool                   did_send{};
    nlohmann::json         broadcast_message_value;
    bool                   did_broadcast{};

    void subscribe(topic_e topic, const web_server::callback_t& callback) final
    {
        subscribed_topic = topic;
        subscription     = callback;
    }

    void set_config_getters([[maybe_unused]] const web_server::config_getters_t& getters) final {}
    void start([[maybe_unused]] uint16_t port, [[maybe_unused]] boost::asio::io_context* service) final {}
    void stop() final {}

    void send_message(const nlohmann::json& message, int64_t connection_id) final
    {
        sent_connection_id = connection_id;
        sent_message_value = message;
        did_send           = true;
    }

    void send_message_sync(const nlohmann::json& message, int64_t connection_id) final
    {
        sent_connection_id = connection_id;
        sent_message_value = message;
        did_send           = true;
    }

    void broadcast_message(const nlohmann::json& message) final
    {
        broadcast_message_value = message;
        did_broadcast           = true;
    }
    void broadcast_message_sync(const nlohmann::json& message) final
    {
        broadcast_message_value = message;
        did_broadcast           = true;
    }
};

TEST(web_message, typed_broadcast_serializes_the_envelope_and_payload)
{
    server_mock_s         implementation;
    web_server::server_s& server  = implementation;
    const nlohmann::json  options = {
        {"name", "Example"},
        {"gain", 0.5      }
    };

    server.broadcast_message_sync(web_message::add_node_command_s{
        .origin_id = 42,
        .node =
            {
                   .type    = "ndi_output",
                   .id      = "output-1",
                   .options = options,
                   },
    });

    ASSERT_TRUE(implementation.did_broadcast);
    EXPECT_EQ(implementation.broadcast_message_value,
              nlohmann::json({
                  {"action",    "command" },
                  {"topic",     "add_node"},
                  {"origin_id", 42        },
                  {"node",
                   {
                       {"type", "ndi_output"},
                       {"id", "output-1"},
                       {"options", options},
                   }                      },
    }));
}

TEST(web_message, configuration_result_uses_the_shared_configuration_contract)
{
    const nlohmann::json snapshot = {
        {"schema_version", 1                                    },
        {"nodes",
         {{
             {"type", "ndi_output"},
             {"id", "output-1"},
             {"schema_version", 2},
             {"options", {{"name", "Program"}}},
         }}                                                     },
        {"connections",    nlohmann::json::array()              },
        {"status",         {{"output-1", {{"connected", true}}}}},
    };

    const auto message = web_message::config_result_s{
        .token  = "request-1",
        .config = snapshot.get<web_message::config_s>(),
    };

    EXPECT_EQ(nlohmann::json(message),
              nlohmann::json({
                  {"action", "result"   },
                  {"token",  "request-1"},
                  {"config", snapshot   },
    }));
}

TEST(web_message, typed_subscription_owns_values_after_dispatch)
{
    server_mock_s                      implementation;
    web_server::server_s&              server = implementation;
    web_message::update_node_request_s received;
    int64_t                            received_connection_id{};
    bool                               received_message{};

    server.subscribe<web_message::update_node_request_s>(
        topic_e::update_node, [&](const web_message::update_node_request_s& message, int64_t connection_id) {
            received               = message;
            received_connection_id = connection_id;
            received_message       = true;
        });

    ASSERT_TRUE(implementation.subscription);
    nlohmann::json payload = {
        {"action",  "command"       },
        {"topic",   "update_node"   },
        {"token",   "request-1"     },
        {"id",      "node-1"        },
        {"options", {{"gain", 0.75}}},
    };
    implementation.subscription(std::move(payload), 7);

    EXPECT_EQ(implementation.subscribed_topic, topic_e::update_node);
    EXPECT_EQ(received_connection_id, 7);
    ASSERT_TRUE(received_message);
    EXPECT_EQ(received.token, std::optional<std::string>{"request-1"});
    EXPECT_EQ(received.id, "node-1");
    EXPECT_EQ(received.options,
              nlohmann::json({
                  {"gain", 0.75}
    }));
}

TEST(web_message, malformed_typed_subscription_returns_a_typed_error)
{
    server_mock_s         implementation;
    web_server::server_s& server = implementation;
    bool                  invoked{};

    server.subscribe<web_message::remove_node_request_s>(
        topic_e::remove_node, [&](const web_message::remove_node_request_s&, int64_t) { invoked = true; });

    ASSERT_TRUE(implementation.subscription);
    implementation.subscription(nlohmann::json({
                                    {"action", "command"    },
                                    {"topic",  "remove_node"},
                                    {"token",  "request-2"  },
    }),
                                9);

    EXPECT_FALSE(invoked);
    ASSERT_TRUE(implementation.did_send);
    EXPECT_EQ(implementation.sent_connection_id, 9);
    EXPECT_EQ(implementation.sent_message_value,
              nlohmann::json({
                  {"action", "error"            },
                  {"token",  "request-2"        },
                  {"error",  "malformed_payload"},
    }));
}

TEST(web_message, typed_subscription_reports_handler_failures_as_internal_errors)
{
    server_mock_s         implementation;
    web_server::server_s& server = implementation;

    server.subscribe<web_message::remove_node_request_s>(topic_e::remove_node,
                                                         [](const auto&, int64_t) {
                                                             throw std::runtime_error("handler failed");
                                                         });

    ASSERT_TRUE(implementation.subscription);
    implementation.subscription(nlohmann::json({
                                    {"action", "command"    },
                                    {"topic",  "remove_node"},
                                    {"token",  "request-3"  },
                                    {"id",     "node-1"     },
                                }),
                                11);

    ASSERT_TRUE(implementation.did_send);
    EXPECT_EQ(implementation.sent_connection_id, 11);
    EXPECT_EQ(implementation.sent_message_value,
              nlohmann::json({
                  {"action", "error"         },
                  {"token",  "request-3"     },
                  {"error",  "internal_error"},
              }));
}

TEST(web_message, typed_subscription_rejects_a_mismatched_registered_topic)
{
    server_mock_s         implementation;
    web_server::server_s& server = implementation;

    EXPECT_THROW(server.subscribe<web_message::remove_node_request_s>(topic_e::add_node, [](const auto&, int64_t) {}),
                 std::logic_error);
    EXPECT_FALSE(implementation.subscription);
}

} // namespace
