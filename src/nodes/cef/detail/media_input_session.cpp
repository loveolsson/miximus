#include "media_input_session.hpp"

#include "command_messages.hpp"
#include "gpu/device.hpp"
#include "media_input_exports.hpp"
#include "media_input_renderer.hpp"
#include "task.hpp"
#include "wrapper/cef/media_input_abi.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <dlfcn.h>
#include <format>
#include <mutex>
#include <thread>

namespace miximus::nodes::cef::detail {
namespace {
using namespace std::chrono_literals;
constexpr size_t INPUTS = 8, EXPORT_DEPTH = 2;
struct budget_s
{
    std::mutex              mutex;
    size_t                  used{};
    static constexpr size_t LIMIT = 2ULL * 1024 * 1024 * 1024;
};
struct reservation_s
{
    std::shared_ptr<budget_s>  budget;
    std::array<size_t, INPUTS> high_water{};
    size_t                     bytes{};
    explicit reservation_s(std::shared_ptr<budget_s> owner)
        : budget(std::move(owner))
    {
    }
    ~reservation_s()
    {
        std::lock_guard lock(budget->mutex);
        budget->used -= bytes;
    }
    bool grow(size_t input, gpu::extent_s extent)
    {
        // Conservative destination allowance: the helper can select up to eight
        // Chromium slots. Keep each input's high-water charge until retirement,
        // since a media consumer can retain a destination from an older size.
        const size_t    padded_width  = (size_t(extent.width) + 255) / 256 * 256;
        const size_t    padded_height = (size_t(extent.height) + 63) / 64 * 64;
        const size_t    amount        = padded_width * padded_height * 4 * (EXPORT_DEPTH + 8);
        std::lock_guard lock(budget->mutex);
        const size_t    delta = std::max(amount, high_water[input]) - high_water[input];
        if (delta > budget_s::LIMIT - budget->used)
            return false;
        budget->used += delta;
        bytes += delta;
        high_water[input] += delta;
        return true;
    }
    size_t reserved() const
    {
        std::lock_guard lock(budget->mutex);
        return bytes;
    }
};
} // namespace
struct media_input_runtime_s::impl_s
{
    std::shared_ptr<budget_s>           budget = std::make_shared<budget_s>();
    media_input_exports_s::quarantine_s quarantine;
};
media_input_runtime_s::media_input_runtime_s()
    : impl_(std::make_unique<impl_s>())
{
}
media_input_runtime_s::~media_input_runtime_s() = default;

struct media_input_session_s::impl_s
{
    struct state_s : std::enable_shared_from_this<state_s>
    {
        struct input_s
        {
            gpu::extent_s wanted{16, 16};
            std::string   source_node, source_interface, error;
            uint64_t      revision{1}, configured{}, sent_generation{};
            bool          invalidation_pending{};
        };
        struct pending_s
        {
            std::shared_ptr<media_input_exports_s::frame_s> frame;
            std::chrono::steady_clock::time_point           deadline;
        };
        std::shared_ptr<media_input_runtime_s>       runtime;
        std::shared_ptr<reservation_s>               reservation;
        std::unique_ptr<media_input_exports_s>       exports;
        gpu::texture_s                               black;
        cef_wrapper::send_media_frame_t              api{};
        mutable std::mutex                           mutex;
        std::array<input_s, INPUTS>                  inputs;
        std::array<pending_s, INPUTS * EXPORT_DEPTH> pending;
        std::string                                  context, error;
        int                                          browser_id{};
        uint32_t                                     subscribed{};
        uint64_t                                     submitted{}, delivered{}, transport_drops{};
        bool                                         closing{}, drained{};

        state_s(gpu::device_s& device, std::shared_ptr<media_input_runtime_s> owner)
            : runtime(std::move(owner))
        {
            api = reinterpret_cast<cef_wrapper::send_media_frame_t>(dlsym(RTLD_DEFAULT, cef_wrapper::SEND_MEDIA_FRAME));
            if (!runtime || !api) {
                error = "CEF runtime does not provide GPU media input v2";
                return;
            }
            reservation = std::make_shared<reservation_s>(runtime->impl_->budget);
            exports     = std::make_unique<media_input_exports_s>(
                device, EXPORT_DEPTH, runtime->impl_->quarantine, 128ULL * 1024 * 1024, reservation);
            black         = device.create_texture({16, 16});
            auto setup    = device.create_recording_context(1);
            auto commands = setup.try_record();
            commands->clear(black, {0, 0, 0, 1});
            if (commands->submit().wait(5s) != gpu::wait_result_e::ready)
                throw std::runtime_error("Cannot initialize disconnected browser input");
        }
        void revoke_locked()
        {
            subscribed = 0;
            context.clear();
            if (exports)
                for (size_t input = 0; input < INPUTS; ++input) {
                    exports->invalidate(input);
                    ++inputs[input].revision;
                }
        }
        void complete(const std::shared_ptr<media_input_exports_s::frame_s>& frame, bool safe, bool accepted)
        {
            {
                std::lock_guard lock(mutex);
                auto&           slot = pending[frame->ticket().input * EXPORT_DEPTH + frame->ticket().slot];
                if (slot.frame != frame)
                    return;
                slot = {};
                delivered += accepted;
                transport_drops += !accepted;
                if (!safe)
                    error = "Input GPU retirement was not established; buffers quarantined";
            }
            frame->retire(safe);
        }
        void post_frame(std::shared_ptr<media_input_exports_s::frame_s> frame)
        {
            const auto                 d      = frame->image().descriptor();
            const auto                 ticket = frame->ticket();
            cef_wrapper::media_frame_s packet{.input             = static_cast<uint32_t>(ticket.input),
                                              .fd                = d.fd,
                                              .width             = d.extent.width,
                                              .height            = d.extent.height,
                                              .stride            = static_cast<uint32_t>(d.stride),
                                              .offset            = d.offset,
                                              .modifier          = d.modifier,
                                              .allocation_bytes  = frame->image().allocation_bytes(),
                                              .timestamp_us      = frame->timestamp_us(),
                                              .source_generation = ticket.generation};
            std::string                token;
            int                        browser{};
            {
                std::lock_guard lock(mutex);
                token                                              = context;
                browser                                            = browser_id;
                pending[ticket.input * EXPORT_DEPTH + ticket.slot] = {frame, std::chrono::steady_clock::now() + 10s};
            }
            auto self = shared_from_this();
            if (!CefPostTask(TID_UI, new task_s([self, frame, packet, token, browser] {
                                 bool current{};
                                 {
                                     std::lock_guard lock(self->mutex);
                                     current =
                                         !self->closing && self->context == token && !token.empty() && frame->current();
                                 }
                                 if (!current) {
                                     self->complete(frame, true, false);
                                     return;
                                 }
                                 struct callback_s
                                 {
                                     std::shared_ptr<state_s>                        state;
                                     std::shared_ptr<media_input_exports_s::frame_s> frame;
                                 };
                                 auto*      callback = new callback_s{self, frame};
                                 const auto done     = [](void* pointer, int safe, int accepted) {
                                     std::unique_ptr<callback_s> callback(static_cast<callback_s*>(pointer));
                                     callback->state->complete(callback->frame, safe != 0, accepted != 0);
                                 };
                                 if (!self->api(browser, token.c_str(), &packet, done, callback))
                                     done(callback, 1, 0);
                             })))
                complete(frame, true, false);
        }
        void invalidate_renderer(size_t input)
        {
            std::lock_guard lock(mutex);
            if (closing || context.empty() || inputs[input].invalidation_pending ||
                inputs[input].sent_generation == exports->generation(input))
                return;
            inputs[input].invalidation_pending = true;
            auto self                          = shared_from_this();
            if (!CefPostTask(TID_UI, new task_s([self, input] {
                                 cef_wrapper::media_frame_s packet{.input             = static_cast<uint32_t>(input),
                                                                   .source_generation = 1,
                                                                   .invalidate_only   = 1};
                                 std::string                token;
                                 int                        browser{};
                                 {
                                     std::lock_guard lock(self->mutex);
                                     token                    = self->context;
                                     browser                  = self->browser_id;
                                     packet.source_generation = self->exports->generation(input);
                                 }
                                 if (!token.empty()) {
                                     const auto done = [](void*, int, int) {};
                                     self->api(browser, token.c_str(), &packet, done, nullptr);
                                 }
                                 std::lock_guard lock(self->mutex);
                                 self->inputs[input].invalidation_pending = false;
                                 if (self->context == token)
                                     self->inputs[input].sent_generation = packet.source_generation;
                             })))
                inputs[input].invalidation_pending = false;
        }
        void configure(size_t input)
        {
            gpu::extent_s wanted;
            uint64_t      revision{};
            {
                std::lock_guard lock(mutex);
                auto&           entry = inputs[input];
                if (closing || !(subscribed & (1u << input)) || entry.configured == entry.revision ||
                    !entry.error.empty())
                    return;
                wanted   = entry.wanted;
                revision = entry.revision;
            }
            try {
                if (wanted.width > 4096 || wanted.height > 4096 || !reservation->grow(input, wanted))
                    throw std::runtime_error("Input dimensions or shared texture budget exceeded");
                if (!exports->configure(input, wanted))
                    return;
                std::lock_guard lock(mutex);
                if (inputs[input].revision == revision)
                    inputs[input].configured = revision;
            } catch (const std::exception& failure) {
                std::lock_guard lock(mutex);
                if (inputs[input].revision == revision)
                    inputs[input].error = failure.what();
            }
        }
        void run(std::stop_token stop)
        {
            try {
                while (!stop.stop_requested()) {
                    std::array<std::shared_ptr<media_input_exports_s::frame_s>, INPUTS * EXPORT_DEPTH> expired;
                    {
                        std::lock_guard lock(mutex);
                        for (size_t slot = 0; slot < pending.size(); ++slot)
                            if (pending[slot].frame && pending[slot].deadline < std::chrono::steady_clock::now())
                                expired[slot] = pending[slot].frame;
                    }
                    for (auto& frame : expired)
                        if (frame)
                            complete(frame, false, false);
                    if (exports->failed()) {
                        std::lock_guard lock(mutex);
                        drained = true;
                        return;
                    }
                    // Drain revoked/native work before attempting replacement allocations.
                    while (auto frame = exports->poll())
                        post_frame(std::move(frame));
                    for (size_t input = 0; input < INPUTS; ++input) {
                        invalidate_renderer(input);
                        configure(input);
                    }
                    {
                        std::lock_guard lock(mutex);
                        if (closing && exports->idle()) {
                            drained = true;
                            return;
                        }
                    }
                    std::this_thread::sleep_for(1ms);
                }
            } catch (const std::exception& failure) {
                std::lock_guard lock(mutex);
                error = failure.what();
                revoke_locked();
                drained = true;
            }
        }
    };
    std::shared_ptr<state_s> state;
    std::jthread             worker;
    impl_s(gpu::device_s& device, std::shared_ptr<media_input_runtime_s> runtime)
        : state(std::make_shared<state_s>(device, std::move(runtime)))
    {
        if (state->exports)
            worker = std::jthread([state = state](std::stop_token stop) { state->run(stop); });
    }
};

media_input_session_s::media_input_session_s(gpu::device_s& device, std::shared_ptr<media_input_runtime_s> runtime)
    : impl_(std::make_unique<impl_s>(device, std::move(runtime)))
{
}
media_input_session_s::~media_input_session_s() { close(); }
void media_input_session_s::attach(int browser_id)
{
    std::lock_guard lock(impl_->state->mutex);
    impl_->state->browser_id = browser_id;
}
void media_input_session_s::revoke_context()
{
    std::lock_guard lock(impl_->state->mutex);
    impl_->state->revoke_locked();
}
void media_input_session_s::close()
{
    std::lock_guard lock(impl_->state->mutex);
    if (!impl_->state->closing) {
        impl_->state->closing = true;
        impl_->state->revoke_locked();
    }
}
uint32_t media_input_session_s::demand() const
{
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->exports && !impl_->state->closing && !impl_->state->drained ? impl_->state->subscribed : 0;
}
bool media_input_session_s::idle() const
{
    std::lock_guard lock(impl_->state->mutex);
    return !impl_->state->exports || impl_->state->drained;
}
bool media_input_session_s::receive(const CefRefPtr<CefBrowser>&        browser,
                                    const CefRefPtr<CefFrame>&          frame,
                                    CefProcessId                        source,
                                    const CefRefPtr<CefProcessMessage>& message)
{
    if (!browser || !frame || !message || source != PID_RENDERER || !frame->IsMain())
        return false;
    const auto main_frame = browser->GetMainFrame();
    if (!main_frame || frame->GetIdentifier() != main_frame->GetIdentifier())
        return false;
    auto&           state = *impl_->state;
    std::lock_guard lock(state.mutex);
    const auto      name    = message->GetName();
    const auto      args    = message->GetArgumentList();
    const auto      context = command_protocol::decode_context(args);
    if (name == command_protocol::CONTEXT_READY && context) {
        state.revoke_locked();
        if (!state.closing)
            state.context = context->token;
        return true;
    }
    if (name == command_protocol::CONTEXT_RELEASED && context) {
        if (state.context == context->token)
            state.revoke_locked();
        return true;
    }
    if (name != MEDIA_INPUT_SUBSCRIBE)
        return false;
    if (state.exports && !state.closing && args->GetSize() == 2 && args->GetType(0) == VTYPE_STRING &&
        args->GetString(0).ToString() == state.context && args->GetType(1) == VTYPE_INT && args->GetInt(1) >= 0 &&
        args->GetInt(1) < 8) {
        const auto input = static_cast<size_t>(args->GetInt(1));
        state.exports->invalidate(input);
        state.subscribed |= 1u << input;
        ++state.inputs[input].revision;
        state.inputs[input].error.clear();
    }
    return true;
}
std::function<void()> media_input_session_s::record(size_t                input,
                                                    gpu::recording_s&     commands,
                                                    const gpu::texture_s* source,
                                                    std::string_view      source_node,
                                                    std::string_view      source_interface,
                                                    int64_t               timestamp_us)
{
    if (input >= INPUTS)
        throw std::out_of_range("Invalid browser input index");
    auto& state = *impl_->state;
    {
        std::lock_guard lock(state.mutex);
        if (!state.exports || state.closing || state.drained || !(state.subscribed & (1u << input)))
            return {};
        auto&      entry  = state.inputs[input];
        const auto extent = source ? source->extent() : entry.wanted;
        if (entry.wanted != extent || entry.source_node != source_node || entry.source_interface != source_interface) {
            state.exports->invalidate(input);
            entry.wanted           = extent;
            entry.source_node      = source_node;
            entry.source_interface = source_interface;
            entry.error.clear();
            ++entry.revision;
        }
        if (entry.configured != entry.revision || !entry.error.empty())
            return {};
    }
    auto publication = state.exports->record(input, commands, source ? *source : state.black, timestamp_us);
    if (!publication)
        return {};
    return [publication, owner = impl_->state] {
        publication->commit();
        std::lock_guard lock(owner->mutex);
        ++owner->submitted;
    };
}
media_input_metrics_s media_input_session_s::metrics() const
{
    auto&                 state = *impl_->state;
    std::lock_guard       lock(state.mutex);
    media_input_metrics_s result{.available = bool(state.exports),
                                 .failed =
                                     state.exports && (state.exports->failed() || (state.drained && !state.closing)),
                                 .subscribed     = state.subscribed,
                                 .submitted      = state.submitted,
                                 .delivered      = state.delivered,
                                 .drops          = state.transport_drops,
                                 .reserved_bytes = state.reservation ? state.reservation->reserved() : 0,
                                 .error          = state.error};
    if (state.exports)
        for (size_t input = 0; input < INPUTS; ++input) {
            const auto metrics = state.exports->metrics(input);
            result.drops += metrics.capacity_drops;
            result.occupied += metrics.occupied;
            if (result.error.empty() && !state.inputs[input].error.empty())
                result.error = std::format("Input {}: {}", input, state.inputs[input].error);
        }
    return result;
}
} // namespace miximus::nodes::cef::detail
