#include "renderer_app.hpp"

#include "command_protocol.hpp"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace miximus::nodes::cef::detail {
namespace {
namespace protocol = command_protocol;

void send_context(CefRefPtr<CefFrame> frame, const char* message_name, const std::string& token)
{
    auto message = CefProcessMessage::Create(message_name);
    message->GetArgumentList()->SetString(0, token);
    frame->SendProcessMessage(PID_BROWSER, message);
}

class result_handler_s;

struct context_state_s
{
    std::string                                        token;
    CefRefPtr<CefV8Context>                            context;
    CefRefPtr<CefV8Value>                              invoke;
    CefRefPtr<CefV8Value>                              timing_handler;
    bool                                               active{true};
    std::map<std::string, CefRefPtr<result_handler_s>> pending;
};

class result_handler_s final : public CefV8Handler
{
    std::weak_ptr<context_state_s> context_;
    std::string                    request_;
    std::string                    generation_;
    bool                           delivered_{};
    IMPLEMENT_REFCOUNTING(result_handler_s);

  public:
    result_handler_s(std::shared_ptr<context_state_s> context, std::string request, std::string generation)
        : context_(context)
        , request_(std::move(request))
        , generation_(std::move(generation))
    {
    }

    void deliver(bool success, std::string json)
    {
        const auto context = context_.lock();
        if (delivered_ || !context || !context->active)
            return;
        delivered_ = true;
        if (json.size() > protocol::MAX_JSON_BYTES) {
            success = false;
            json    = "JavaScript result exceeds the JSON response limit";
        }
        auto message = CefProcessMessage::Create(protocol::RESULT);
        auto args    = message->GetArgumentList();
        args->SetString(0, request_);
        args->SetString(1, generation_);
        args->SetString(2, context->token);
        args->SetBool(3, success);
        args->SetString(4, json);
        context->context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
        context->pending.erase(request_);
    }

    bool Execute(const CefString&,
                 CefRefPtr<CefV8Value>,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>&,
                 CefString&) override
    {
        if (arguments.size() == 2 && arguments[0]->IsBool() && arguments[1]->IsString())
            deliver(arguments[0]->GetBoolValue(), arguments[1]->GetStringValue().ToString());
        return true;
    }
};

class renderer_app_s final
    : public CefApp
    , public CefRenderProcessHandler
{
    std::map<int, std::shared_ptr<context_state_s>> contexts_;
    uint64_t                                        next_context_{};
    IMPLEMENT_REFCOUNTING(renderer_app_s);

  public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void
    OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
    {
        if (!frame->IsMain())
            return;
        auto state     = std::make_shared<context_state_s>();
        state->token   = frame->GetIdentifier().ToString() + ":" + std::to_string(++next_context_);
        state->context = context;
        CefRefPtr<CefV8Exception> exception;
        // The native callback is an argument, never installed on window. Promise
        // completion is explicit; neither execution nor its reply implies paint.
        constexpr char invoke[] = R"JS((function(fn, payload, reply, discardResult) {
            const fail = error => {
                try { reply(false, String(error)); }
                catch (_) { reply(false, 'JavaScript exception could not be serialized'); }
            };
            try {
                Promise.resolve(fn(JSON.parse(payload))).then(value => {
                    try {
                        const json = discardResult ? "null" : JSON.stringify(value);
                        if (typeof json !== 'string') throw new Error('Result is not JSON serializable');
                        reply(true, json);
                    } catch (error) { fail(error); }
                }, fail);
            } catch (error) { fail(error); }
        }))JS";
        if (!context->Eval(invoke, "miximus-internal-command", 1, state->invoke, exception))
            return;
        contexts_[browser->GetIdentifier()] = state;
        send_context(frame, protocol::CONTEXT_READY, state->token);
    }

    void OnContextReleased(CefRefPtr<CefBrowser>   browser,
                           CefRefPtr<CefFrame>     frame,
                           CefRefPtr<CefV8Context> context) override
    {
        if (!frame->IsMain())
            return;
        const auto found = contexts_.find(browser->GetIdentifier());
        if (found == contexts_.end() || !found->second->context->IsSame(context))
            return;
        found->second->active = false;
        send_context(frame, protocol::CONTEXT_RELEASED, found->second->token);
        contexts_.erase(found);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>        browser,
                                  CefRefPtr<CefFrame>          frame,
                                  CefProcessId                 source,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        if (source != PID_BROWSER || !frame->IsMain() ||
            (message->GetName() != protocol::REQUEST && message->GetName() != protocol::CANCEL))
            return false;
        const auto args  = message->GetArgumentList();
        const auto found = contexts_.find(browser->GetIdentifier());
        if (message->GetName() == protocol::CANCEL) {
            if (args->GetSize() == 2 && found != contexts_.end() && args->GetString(1) == found->second->token) {
                const auto pending = found->second->pending.find(args->GetString(0).ToString());
                if (pending != found->second->pending.end()) {
                    // deliver() removes the pending entry; retain the handler
                    // through that removal and acknowledge transport retirement.
                    const auto result = pending->second;
                    result->deliver(false, "Command cancelled");
                }
            }
            return true;
        }
        if (args->GetSize() != 6)
            return true;
        if (found == contexts_.end() || args->GetString(2) != found->second->token)
            return true; // Browser-side navigation cancellation/timeout owns settlement.
        const auto                  state = found->second;
        CefRefPtr<result_handler_s> result =
            new result_handler_s(state, args->GetString(0).ToString(), args->GetString(1).ToString());
        if (state->pending.size() >= protocol::MAX_PENDING) {
            result->deliver(false, "Renderer command capacity exhausted");
            return true;
        }
        state->pending.emplace(args->GetString(0).ToString(), result);
        const auto function_source = args->GetString(3).ToString();
        const auto payload         = args->GetString(4).ToString();
        if (function_source.size() > protocol::MAX_SOURCE_BYTES || payload.size() > protocol::MAX_JSON_BYTES) {
            result->deliver(false, "Command exceeds the payload limit");
            return true;
        }
        if (!state->context->Enter()) {
            result->deliver(false, "JavaScript context is unavailable");
            return true;
        }
        CefRefPtr<CefV8Value>     function;
        CefRefPtr<CefV8Exception> exception;
        const auto                kind = static_cast<protocol::request_kind_e>(args->GetInt(5));
        if (kind == protocol::request_kind_e::program_time)
            function = state->timing_handler;
        const bool evaluated =
            kind == protocol::request_kind_e::program_time ||
            state->context->Eval("(" + function_source + ")", "miximus-internal-command", 1, function, exception);
        if (!evaluated || !function || !function->IsFunction()) {
            result->deliver(false,
                            exception ? exception->GetMessage().ToString() : "Command must evaluate to a function");
        } else if (kind == protocol::request_kind_e::timing_handler) {
            state->timing_handler = function;
            result->deliver(true, "null");
        } else {
            state->invoke->ExecuteFunction(nullptr,
                                           {
                                               function,
                                               CefV8Value::CreateString(payload),
                                               CefV8Value::CreateFunction("reply", result),
                                               CefV8Value::CreateBool(kind == protocol::request_kind_e::program_time),
                                           });
            if (state->invoke->HasException()) {
                result->deliver(false, state->invoke->GetException()->GetMessage().ToString());
                state->invoke->ClearException();
            }
        }
        state->context->Exit();
        return true;
    }
};
} // namespace

CefRefPtr<CefApp> create_renderer_app() { return new renderer_app_s; }
} // namespace miximus::nodes::cef::detail
