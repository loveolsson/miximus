#pragma once

#include "command_protocol.hpp"
#include "include/cef_process_message.h"

#include <optional>
#include <string>

namespace miximus::nodes::cef::detail::command_protocol {

struct request_s
{
    std::string    id;
    std::string    generation;
    std::string    context;
    std::string    source;
    std::string    json;
    request_kind_e kind{};
};
struct result_s
{
    std::string id;
    std::string generation;
    std::string context;
    bool        success{};
    std::string payload;
};
struct cancel_s
{
    std::string id;
    std::string context;
};
struct context_s
{
    std::string token;
};

inline CefRefPtr<CefProcessMessage> encode(const request_s& value)
{
    auto message = CefProcessMessage::Create(REQUEST);
    auto args    = message->GetArgumentList();
    args->SetString(0, value.id);
    args->SetString(1, value.generation);
    args->SetString(2, value.context);
    args->SetString(3, value.source);
    args->SetString(4, value.json);
    args->SetInt(5, static_cast<int>(value.kind));
    return message;
}

inline std::optional<request_s> decode_request(const CefRefPtr<CefListValue>& args)
{
    if (args->GetSize() != 6) {
        return {};
    }
    for (size_t index = 0; index < 5; ++index) {
        if (args->GetType(index) != VTYPE_STRING) {
            return {};
        }
    }
    if (args->GetType(5) != VTYPE_INT) {
        return {};
    }
    const auto kind = args->GetInt(5);
    if (kind < static_cast<int>(request_kind_e::custom) || kind > static_cast<int>(request_kind_e::program_time)) {
        return {};
    }
    return request_s{args->GetString(0).ToString(),
                     args->GetString(1).ToString(),
                     args->GetString(2).ToString(),
                     args->GetString(3).ToString(),
                     args->GetString(4).ToString(),
                     static_cast<request_kind_e>(kind)};
}

inline CefRefPtr<CefProcessMessage> encode(const result_s& value)
{
    auto message = CefProcessMessage::Create(RESULT);
    auto args    = message->GetArgumentList();
    args->SetString(0, value.id);
    args->SetString(1, value.generation);
    args->SetString(2, value.context);
    args->SetBool(3, value.success);
    args->SetString(4, value.payload);
    return message;
}

inline std::optional<result_s> decode_result(const CefRefPtr<CefListValue>& args)
{
    if (args->GetSize() != 5 || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_STRING ||
        args->GetType(2) != VTYPE_STRING || args->GetType(3) != VTYPE_BOOL || args->GetType(4) != VTYPE_STRING) {
        return {};
    }
    return result_s{args->GetString(0).ToString(),
                    args->GetString(1).ToString(),
                    args->GetString(2).ToString(),
                    args->GetBool(3),
                    args->GetString(4).ToString()};
}

inline CefRefPtr<CefProcessMessage> encode(const cancel_s& value)
{
    auto message = CefProcessMessage::Create(CANCEL);
    auto args    = message->GetArgumentList();
    args->SetString(0, value.id);
    args->SetString(1, value.context);
    return message;
}

inline std::optional<cancel_s> decode_cancel(const CefRefPtr<CefListValue>& args)
{
    if (args->GetSize() != 2 || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_STRING) {
        return {};
    }
    return cancel_s{args->GetString(0).ToString(), args->GetString(1).ToString()};
}

inline CefRefPtr<CefProcessMessage> encode(const context_s& value, const char* name)
{
    auto message = CefProcessMessage::Create(name);
    message->GetArgumentList()->SetString(0, value.token);
    return message;
}

inline std::optional<context_s> decode_context(const CefRefPtr<CefListValue>& args)
{
    if (args->GetSize() != 1 || args->GetType(0) != VTYPE_STRING) {
        return {};
    }
    return context_s{args->GetString(0).ToString()};
}
} // namespace miximus::nodes::cef::detail::command_protocol
