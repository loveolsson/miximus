#pragma once
#include <nlohmann/json.hpp>

#include <cassert>
#include <string_view>

namespace miximus::message {

enum class action_t {
  invalid = -1,
  subscribe = 0,
  unsubscribe,
  ping,
  socket_info,
  command,
  result,
  error,
  _count,
};

enum class topic_t {
  invalid = -1,
  settings = 0,
  _count,
};

enum class error_t {
  no_error = -1,
  internal_error = 0,
  invalid_topic,
  _count,
};

typedef std::string token_t;
typedef std::string_view token_ref_t;

typedef std::function<void(action_t, nlohmann::json &&, int64_t)> callback_t;

static inline action_t action_from_string(std::string_view a) {
  action_t res = action_t::invalid;

  if (a == "subscribe") {
    res = action_t::subscribe;
  } else if (a == "unsubscribe") {
    res = action_t::unsubscribe;
  } else if (a == "ping") {
    res = action_t::ping;
  } else if (a == "socket_info") {
    res = action_t::socket_info;
  } else if (a == "command") {
    res = action_t::command;
  } else if (a == "result") {
    res = action_t::result;
  } else if (a == "error") {
    res = action_t::error;
  }

  return res;
}

constexpr std::string_view get_action_string(action_t action) {
  switch (action) {
  case action_t::subscribe:
    return "subscribe";
  case action_t::unsubscribe:
    return "unsubscribe";
  case action_t::ping:
    return "ping";
  case action_t::socket_info:
    return "socket_info";
  case action_t::command:
    return "command";
  case action_t::result:
    return "result";
  case action_t::error:
    return "error";

  case action_t::invalid:
  default:
    assert(false);
    return "internal_error";
  }
}

static inline action_t get_action_from_payload(const nlohmann::json &payload) {
  auto act = payload.find("action");
  if (act == payload.cend() || !act->is_string()) {
    return action_t::invalid;
  }

  return action_from_string(act->get<std::string_view>());
}

static inline topic_t topic_from_string(std::string_view a) {
  topic_t res = topic_t::invalid;

  if (a == "settings") {
    res = topic_t::settings;
  }

  return res;
}

static inline topic_t get_topic_from_payload(const nlohmann::json &payload) {
  auto top = payload.find("topic");
  if (top == payload.cend() || !top->is_string()) {
    return topic_t::invalid;
  }

  return topic_from_string(top->get<std::string_view>());
}

constexpr std::string_view get_error_string(error_t error) {
  switch (error) {
  case error_t::no_error:
    return "no_error";
  case error_t::invalid_topic:
    return "invalid_topic";

  case error_t::internal_error:
  default:
    return "internal_error";
  }
}

static inline token_ref_t
get_token_from_payload(const nlohmann::json &payload) {
  auto token = payload.find("token");
  if (token == payload.cend() || !token->is_string()) {
    return {};
  }

  return token->get<token_ref_t>();
}

static inline nlohmann::json create_ping_response_payload() {
  return {{"action", get_action_string(action_t::ping)}, {"response", true}};
}

static inline nlohmann::json create_socket_info_payload(int64_t id) {
  return {{"action", get_action_string(action_t::socket_info)}, {"id", id}};
}

static inline nlohmann::json create_command_base_payload(token_ref_t token) {
  return {{"action", get_action_string(action_t::command)}, {"token", token}};
}

static inline nlohmann::json create_result_base_payload(token_ref_t token) {
  return {{"action", get_action_string(action_t::result)}, {"token", token}};
}

static inline nlohmann::json create_error_base_payload(token_ref_t token,
                                                       error_t error) {
  return {{"action", get_action_string(action_t::error)},
          {"token", token},
          {"error", get_error_string(error)}};
}

} // namespace miximus::message