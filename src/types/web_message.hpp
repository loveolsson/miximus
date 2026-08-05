#pragma once
#include "action.hpp"
#include "connection.hpp"
#include "error.hpp"
#include "topic.hpp"

#include <boost/describe.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace miximus {

enum class font_registry_command_e
{
    refresh,
};

BOOST_DESCRIBE_ENUM(action_e, subscribe, unsubscribe, ping, socket_info, command, result, error)
BOOST_DESCRIBE_ENUM(topic_e,
                    add_node,
                    remove_node,
                    add_connection,
                    remove_connection,
                    update_node,
                    font_registry,
                    config,
                    node_status)
BOOST_DESCRIBE_ENUM(error_e,
                    no_error,
                    internal_error,
                    malformed_payload,
                    invalid_topic,
                    invalid_type,
                    duplicate_id,
                    invalid_options,
                    not_found,
                    circular_connection)
BOOST_DESCRIBE_ENUM(font_registry_command_e, refresh)

} // namespace miximus

namespace miximus::web_message {

struct message_s
{
    action_e                   action{};
    std::optional<std::string> token{};
};

struct command_s
{
    action_e                   action{action_e::command};
    std::optional<std::string> token{};
    topic_e                    topic{};
    std::optional<int64_t>     origin_id{};
};

struct node_s
{
    std::string             type;
    std::string             id;
    std::optional<uint32_t> schema_version{};
    nlohmann::json          options;
};

using node_status_map_t = std::unordered_map<std::string, nlohmann::json>;

struct config_s
{
    uint32_t                         schema_version{};
    std::vector<node_s>              nodes;
    std::vector<connection_s>        connections;
    std::optional<node_status_map_t> status{};
};

struct ping_response_s
{
    static constexpr action_e action = action_e::ping;

    bool response = true;
};

struct socket_info_s
{
    static constexpr action_e action = action_e::socket_info;

    int64_t     id{};
    std::string bundle_hash;
};

struct result_s
{
    static constexpr action_e action = action_e::result;

    std::string token;
};

struct config_result_s
{
    static constexpr action_e action = action_e::result;

    std::string token;
    config_s    config;
};

struct node_status_result_s
{
    static constexpr action_e action = action_e::result;

    std::string    token;
    std::string    id;
    nlohmann::json status;
};

struct error_s
{
    static constexpr action_e action = action_e::error;

    std::string                token;
    error_e                    error{};
    std::optional<std::string> message{};
};

struct add_node_command_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::add_node;

    int64_t origin_id{};
    node_s  node;
};

struct remove_node_command_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::remove_node;

    int64_t     origin_id{};
    std::string id;
};

struct update_node_command_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::update_node;

    int64_t        origin_id{};
    std::string    id;
    nlohmann::json options;
    bool           has_corrected_values{};
};

struct add_connection_command_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::add_connection;

    int64_t      origin_id{};
    connection_s connection;
};

struct remove_connection_command_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::remove_connection;

    int64_t      origin_id{};
    connection_s connection;
};

struct node_status_command_s
{
    static constexpr action_e action = action_e::command;
    static constexpr topic_e  topic  = topic_e::node_status;

    std::string    id;
    nlohmann::json status;
};

BOOST_DESCRIBE_STRUCT(message_s, (), (action, token))
BOOST_DESCRIBE_STRUCT(command_s, (), (action, token, topic, origin_id))
BOOST_DESCRIBE_STRUCT(node_s, (), (type, id, schema_version, options))
BOOST_DESCRIBE_STRUCT(config_s, (), (schema_version, nodes, connections, status))
BOOST_DESCRIBE_STRUCT(ping_response_s, (), (response))
BOOST_DESCRIBE_STRUCT(socket_info_s, (), (id, bundle_hash))
BOOST_DESCRIBE_STRUCT(result_s, (), (token))
BOOST_DESCRIBE_STRUCT(config_result_s, (), (token, config))
BOOST_DESCRIBE_STRUCT(node_status_result_s, (), (token, id, status))
BOOST_DESCRIBE_STRUCT(error_s, (), (token, error, message))
BOOST_DESCRIBE_STRUCT(add_node_command_s, (), (origin_id, node))
BOOST_DESCRIBE_STRUCT(remove_node_command_s, (), (origin_id, id))
BOOST_DESCRIBE_STRUCT(update_node_command_s, (), (origin_id, id, options, has_corrected_values))
BOOST_DESCRIBE_STRUCT(add_connection_command_s, (), (origin_id, connection))
BOOST_DESCRIBE_STRUCT(remove_connection_command_s, (), (origin_id, connection))
BOOST_DESCRIBE_STRUCT(node_status_command_s, (), (id, status))

} // namespace miximus::web_message
