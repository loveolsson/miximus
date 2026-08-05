#pragma once
#include "web_message.hpp"

#include <optional>
#include <string>

namespace miximus::web_message {

struct subscribe_request_s
{
    static constexpr action_e action = action_e::subscribe;

    std::optional<std::string> token;
    topic_e                    topic{};
};

struct unsubscribe_request_s
{
    static constexpr action_e action = action_e::unsubscribe;

    std::optional<std::string> token;
    topic_e                    topic{};
};

struct add_node_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::add_node;

    std::optional<std::string> token;
    node_s                     node;
};

struct remove_node_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::remove_node;

    std::optional<std::string> token;
    std::string                id;
};

struct update_node_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::update_node;

    std::optional<std::string> token;
    std::string                id;
    nlohmann::json             options;
};

struct add_connection_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::add_connection;

    std::optional<std::string> token;
    connection_s               connection;
};

struct remove_connection_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::remove_connection;

    std::optional<std::string> token;
    connection_s               connection;
};

struct font_registry_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::font_registry;

    std::optional<std::string> token;
    font_registry_command_e    command{};
};

struct config_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::config;

    std::optional<std::string> token;
};

struct node_status_request_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::node_status;

    std::optional<std::string> token;
    std::string                id;
};

BOOST_DESCRIBE_STRUCT(subscribe_request_s, (), (token, topic))
BOOST_DESCRIBE_STRUCT(unsubscribe_request_s, (), (token, topic))
BOOST_DESCRIBE_STRUCT(add_node_request_s, (), (token, node))
BOOST_DESCRIBE_STRUCT(remove_node_request_s, (), (token, id))
BOOST_DESCRIBE_STRUCT(update_node_request_s, (), (token, id, options))
BOOST_DESCRIBE_STRUCT(add_connection_request_s, (), (token, connection))
BOOST_DESCRIBE_STRUCT(remove_connection_request_s, (), (token, connection))
BOOST_DESCRIBE_STRUCT(font_registry_request_s, (), (token, command))
BOOST_DESCRIBE_STRUCT(config_request_s, (), (token))
BOOST_DESCRIBE_STRUCT(node_status_request_s, (), (token, id))

} // namespace miximus::web_message
