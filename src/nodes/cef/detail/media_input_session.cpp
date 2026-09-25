#include "media_input_session.hpp"

#include "command_messages.hpp"
#include "gpu/device.hpp"
#include "media_input_exports.hpp"
#include "media_input_renderer.hpp"
#include "task.hpp"
#include "wrapper/cef/media_input_abi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <format>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace miximus::nodes::cef::detail {
namespace {
using namespace std::chrono_literals;
constexpr size_t INPUTS           = 8;
constexpr size_t MAX_EXPORT_DEPTH = 8;
// Metadata for Chromium-owned transparent content; no Vulkan export is allocated.
constexpr gpu::extent_s DISCONNECTED_EXTENT{.width = 16, .height = 16};

size_t export_depth()
{
    size_t depth = 2;
    // Diagnostic native capacity; no page-controlled allocation growth.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* configured = std::getenv("MIXIMUS_CEF_MEDIA_EXPORT_DEPTH")) {
        const std::string_view value(configured);
        const auto             parsed = std::from_chars(value.data(), value.data() + value.size(), depth);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || depth < 1 ||
            depth > MAX_EXPORT_DEPTH) {
            throw std::invalid_argument("MIXIMUS_CEF_MEDIA_EXPORT_DEPTH must be 1..8");
        }
    }

    return depth;
}

bool is_main_renderer_message(const CefRefPtr<CefBrowser>&        browser,
                              const CefRefPtr<CefFrame>&          frame,
                              CefProcessId                        source,
                              const CefRefPtr<CefProcessMessage>& message)
{
    if (!browser || !frame || !message || source != PID_RENDERER || !frame->IsMain()) {
        return false;
    }

    const auto main_frame = browser->GetMainFrame();
    return main_frame && frame->GetIdentifier() == main_frame->GetIdentifier();
}

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
    size_t                     depth;
    size_t                     bytes{};

    explicit reservation_s(std::shared_ptr<budget_s> owner, size_t export_slots)
        : budget(std::move(owner))
        , depth(export_slots)
    {
    }

    reservation_s(const reservation_s&)            = delete;
    reservation_s& operator=(const reservation_s&) = delete;
    reservation_s(reservation_s&&)                 = delete;
    reservation_s& operator=(reservation_s&&)      = delete;

    ~reservation_s()
    {
        std::scoped_lock lock(budget->mutex);
        budget->used -= bytes;
    }

    bool grow(size_t input, gpu::extent_s extent)
    {
        // Conservative destination allowance: the helper can select up to eight
        // Chromium slots. Keep each input's high-water charge until retirement,
        // since a media consumer can retain a destination from an older size.
        const size_t     padded_width  = (size_t(extent.width) + 255) / 256 * 256;
        const size_t     padded_height = (size_t(extent.height) + 63) / 64 * 64;
        const size_t     amount        = padded_width * padded_height * 4 * (depth + 8);
        std::scoped_lock lock(budget->mutex);
        const size_t     delta = std::max(amount, high_water.at(input)) - high_water.at(input);
        if (delta > budget_s::LIMIT - budget->used) {
            return false;
        }

        budget->used += delta;
        bytes += delta;
        high_water.at(input) += delta;
        return true;
    }

    void restore(size_t input, size_t previous)
    {
        std::scoped_lock lock(budget->mutex);
        const auto       delta = high_water.at(input) - previous;
        budget->used -= delta;
        bytes -= delta;
        high_water.at(input) = previous;
    }

    void release(size_t input) { restore(input, 0); }

    size_t reserved() const
    {
        std::scoped_lock lock(budget->mutex);
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
            gpu::extent_s                         wanted{DISCONNECTED_EXTENT};
            std::string                           source_node;
            std::string                           source_interface;
            std::string                           error;
            uint64_t                              revision{1};
            uint64_t                              configured{};
            uint64_t                              sent_generation{};
            int64_t                               timestamp_us{};
            std::chrono::steady_clock::time_point retry_after;

            bool invalidation_pending{};
            bool transparent_pending{};
            bool transparent{true};
            // Live sources and revoked documents whose destinations have not drained.
            // New GPU allocations wait for previous documents, keeping one bounded pool.
            std::set<std::string, std::less<>> renderer_contexts;
        };

        struct pending_s
        {
            std::shared_ptr<media_input_exports_s::frame_s> frame;
            std::chrono::steady_clock::time_point           deadline;
        };

        std::shared_ptr<media_input_runtime_s> runtime;
        std::shared_ptr<reservation_s>         reservation;
        std::unique_ptr<media_input_exports_s> exports;
        cef_wrapper::send_media_frame_t        api{};

        mutable std::mutex                               mutex;
        std::array<input_s, INPUTS>                      inputs;
        std::array<pending_s, INPUTS * MAX_EXPORT_DEPTH> pending;

        std::string context;
        std::string error;
        int         browser_id{};

        uint32_t subscribed{};
        uint64_t submitted{};
        uint64_t delivered{};
        uint64_t transport_drops{};

        bool closing{};
        bool drained{};
        bool bridge_failed{};

        state_s(gpu::device_s& device, std::shared_ptr<media_input_runtime_s> owner)
            : runtime(std::move(owner))
            , api(reinterpret_cast<cef_wrapper::send_media_frame_t>(dlsym(RTLD_DEFAULT, cef_wrapper::SEND_MEDIA_FRAME)))
        {
            if (!runtime || (api == nullptr)) {
                error = "CEF runtime does not provide GPU media input v3";
                return;
            }

            const auto depth = export_depth();
            reservation      = std::make_shared<reservation_s>(runtime->impl_->budget, depth);
            exports          = std::make_unique<media_input_exports_s>(
                device, depth, runtime->impl_->quarantine, 256ULL * 1024 * 1024, reservation);
        }

        void revoke_locked()
        {
            subscribed = 0;
            context.clear();
            if (exports) {
                for (size_t input = 0; input < INPUTS; ++input) {
                    exports->invalidate(input);
                    ++inputs.at(input).revision;
                    inputs.at(input).transparent = true;
                    inputs.at(input).wanted      = DISCONNECTED_EXTENT;
                    inputs.at(input).source_node.clear();
                    inputs.at(input).source_interface.clear();
                }
            }
        }

        void receive_activity_locked(const CefRefPtr<CefListValue>& args)
        {
            if (exports && !closing && args->GetSize() == 4 && args->GetType(0) == VTYPE_STRING && !context.empty() &&
                args->GetString(0).ToString() == context && args->GetType(1) == VTYPE_INT && args->GetInt(1) >= 0 &&
                args->GetInt(1) < 8 && args->GetType(2) == VTYPE_BOOL && args->GetType(3) == VTYPE_BOOL) {
                const auto input    = static_cast<size_t>(args->GetInt(1));
                auto&      contexts = inputs.at(input).renderer_contexts;
                if (args->GetBool(2)) {
                    contexts.insert(context);
                } else if (args->GetBool(3)) {
                    contexts.erase(context);
                }

                if (!args->GetBool(2) && ((subscribed & (1U << input)) == 0U)) {
                    return; // Destination retirement acknowledgement, not a new activation.
                }

                exports->invalidate(input);
                if (args->GetBool(2)) {
                    subscribed |= 1U << input;
                } else {
                    subscribed &= ~(1U << input);
                }

                inputs.at(input).transparent = true;
                inputs.at(input).wanted      = DISCONNECTED_EXTENT;
                inputs.at(input).source_node.clear();
                inputs.at(input).source_interface.clear();
                ++inputs.at(input).revision;
                inputs.at(input).error.clear();
            }
        }

        void complete(const std::shared_ptr<media_input_exports_s::frame_s>& frame, bool safe, bool accepted)
        {
            {
                std::scoped_lock lock(mutex);
                auto&            slot = pending.at((frame->ticket().input * MAX_EXPORT_DEPTH) + frame->ticket().slot);
                if (slot.frame != frame) {
                    return;
                }

                slot = {};
                delivered += static_cast<uint64_t>(accepted);
                transport_drops += static_cast<uint64_t>(!accepted);
                if (!safe) {
                    error = "Input GPU retirement was not established; buffers quarantined";
                }
            }

            frame->retire(safe);
        }

        void post_frame(const std::shared_ptr<media_input_exports_s::frame_s>& frame)
        {
            const auto descriptor    = frame->image().descriptor();
            const auto ticket        = frame->ticket();
            auto       packet        = cef_wrapper::make_media_frame();
            packet.input             = static_cast<uint32_t>(ticket.input);
            packet.fd                = descriptor.fd;
            packet.width             = descriptor.extent.width;
            packet.height            = descriptor.extent.height;
            packet.stride            = static_cast<uint32_t>(descriptor.stride);
            packet.offset            = descriptor.offset;
            packet.modifier          = descriptor.modifier;
            packet.allocation_bytes  = frame->image().allocation_bytes();
            packet.timestamp_us      = frame->timestamp_us();
            packet.source_generation = ticket.generation;
            std::string token;
            int         browser{};
            {
                std::scoped_lock lock(mutex);
                token                   = context;
                browser                 = browser_id;
                pending.at((ticket.input * MAX_EXPORT_DEPTH) +
                           ticket.slot) = {.frame = frame, .deadline = std::chrono::steady_clock::now() + 10s};
            }

            auto self = shared_from_this();
            auto task = [self, frame, packet, token, browser] {
                bool current{};
                {
                    std::scoped_lock lock(self->mutex);
                    current = !self->closing && self->context == token && !token.empty() && frame->current();
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

                auto*      callback = new callback_s{.state = self, .frame = frame};
                const auto done     = [](void* pointer, int safe, int accepted) {
                    std::unique_ptr<callback_s> callback(static_cast<callback_s*>(pointer));
                    callback->state->complete(callback->frame, safe != 0, accepted != 0);
                };

                if (!self->api(browser, token.c_str(), &packet, done, callback)) {
                    done(callback, 1, 0);
                }
            };

            if (!CefPostTask(TID_UI, new task_s(std::move(task)))) {
                complete(frame, true, false);
            }
        }

        void invalidate_renderer(size_t input)
        {
            std::scoped_lock lock(mutex);
            if (closing || context.empty() || inputs.at(input).invalidation_pending ||
                inputs.at(input).sent_generation == exports->generation(input)) {
                return;
            }

            inputs.at(input).invalidation_pending = true;
            auto self                             = shared_from_this();
            auto task                             = [self, input] {
                auto packet      = cef_wrapper::make_media_frame();
                packet.input     = static_cast<uint32_t>(input);
                packet.operation = 1;
                std::string token;
                int         browser{};
                {
                    std::scoped_lock lock(self->mutex);
                    token                    = self->context;
                    browser                  = self->browser_id;
                    packet.source_generation = self->exports->generation(input);
                }

                if (!token.empty()) {
                    const auto done = [](void*, int, int) {};
                    self->api(browser, token.c_str(), &packet, done, nullptr);
                }

                std::scoped_lock lock(self->mutex);
                self->inputs.at(input).invalidation_pending = false;
                if (self->context == token) {
                    self->inputs.at(input).sent_generation = packet.source_generation;
                }
            };

            if (!CefPostTask(TID_UI, new task_s(std::move(task)))) {
                inputs.at(input).invalidation_pending = false;
            }
        }

        void transparent_frame(size_t input)
        {
            std::scoped_lock lock(mutex);
            auto&            entry = inputs.at(input);
            if (closing || context.empty() || ((subscribed & (1U << input)) == 0U) || !entry.transparent ||
                entry.configured == entry.revision || entry.transparent_pending) {
                return;
            }

            entry.transparent_pending = true;
            auto       self           = shared_from_this();
            const auto revision       = entry.revision;
            const auto token          = context;
            auto       task           = [self, input, revision, token] {
                auto packet      = cef_wrapper::make_media_frame();
                packet.input     = static_cast<uint32_t>(input);
                packet.operation = 2;
                int browser{};
                {
                    std::scoped_lock lock(self->mutex);
                    auto&            entry = self->inputs.at(input);
                    if (self->closing || self->context != token || entry.revision != revision) {
                        entry.transparent_pending = false;
                        return;
                    }

                    packet.timestamp_us      = entry.timestamp_us;
                    browser                  = self->browser_id;
                    packet.source_generation = self->exports->generation(input);
                }

                auto finish = [self, input, revision, token](bool delivered) {
                    std::scoped_lock lock(self->mutex);
                    auto&            entry    = self->inputs.at(input);
                    entry.transparent_pending = false;
                    if (delivered && self->context == token && entry.revision == revision) {
                        entry.configured = revision;
                    }
                };

                using callback_s    = decltype(finish);
                auto*      callback = new callback_s(std::move(finish));
                const auto done     = [](void* user, int, int delivered) {
                    std::unique_ptr<callback_s> callback(static_cast<callback_s*>(user));
                    (*callback)(delivered != 0);
                };

                if (!self->api(browser, token.c_str(), &packet, done, callback)) {
                    done(callback, 1, 0);
                }
            };

            if (!CefPostTask(TID_UI, new task_s(std::move(task)))) {
                entry.transparent_pending = false;
            }
        }

        void configure(size_t input)
        {
            gpu::extent_s wanted;
            uint64_t      revision{};
            {
                std::scoped_lock lock(mutex);
                auto&            entry = inputs.at(input);
                if (closing || ((subscribed & (1U << input)) == 0U) || entry.transparent ||
                    entry.configured == entry.revision ||
                    (!entry.error.empty() && std::chrono::steady_clock::now() < entry.retry_after) ||
                    entry.renderer_contexts.size() != 1 || !entry.renderer_contexts.contains(context)) {
                    return;
                }

                wanted   = entry.wanted;
                revision = entry.revision;
            }

            // Only this worker mutates reservations. Keep charges for earlier
            // successful generations, but undo growth that never reached a consumer.
            const auto previous = reservation->high_water.at(input);
            try {
                if (wanted.width > 4096 || wanted.height > 4096) {
                    throw std::invalid_argument("Input dimensions exceed 4096");
                }
                if (!reservation->grow(input, wanted)) {
                    throw media_input_exports_s::capacity_error_s("Shared input texture budget exhausted");
                }

                if (!exports->configure(input, wanted)) {
                    reservation->restore(input, previous);
                    return;
                }

                std::scoped_lock lock(mutex);
                if (inputs.at(input).revision == revision) {
                    inputs.at(input).configured = revision;
                    inputs.at(input).error.clear();
                } else {
                    exports->invalidate(input); // Stop/source change raced worker allocation.
                }
            } catch (const media_input_exports_s::capacity_error_s& failure) {
                reservation->restore(input, previous);
                std::scoped_lock lock(mutex);
                if (inputs.at(input).revision == revision) {
                    inputs.at(input).error       = failure.what();
                    inputs.at(input).retry_after = std::chrono::steady_clock::now() + 250ms;
                }
            } catch (const std::exception& failure) {
                reservation->restore(input, previous);
                std::scoped_lock lock(mutex);
                if (inputs.at(input).revision == revision) {
                    inputs.at(input).error       = failure.what();
                    inputs.at(input).retry_after = std::chrono::steady_clock::time_point::max();
                }
            }
        }

        void expire_pending_frames()
        {
            std::array<std::shared_ptr<media_input_exports_s::frame_s>, INPUTS * MAX_EXPORT_DEPTH> expired;
            {
                std::scoped_lock lock(mutex);
                for (size_t slot = 0; slot < pending.size(); ++slot) {
                    if (pending.at(slot).frame && pending.at(slot).deadline < std::chrono::steady_clock::now()) {
                        expired.at(slot) = pending.at(slot).frame;
                    }
                }
            }

            for (auto& frame : expired) {
                if (frame) {
                    complete(frame, false, false);
                }
            }
        }

        void run(const std::stop_token& stop)
        {
            try {
                while (!stop.stop_requested()) {
                    expire_pending_frames();

                    if (exports->failed()) {
                        std::scoped_lock lock(mutex);
                        drained = true;
                        return;
                    }

                    // Drain revoked/native work before attempting replacement allocations.
                    while (auto frame = exports->poll()) {
                        post_frame(frame);
                    }

                    for (size_t input = 0; input < INPUTS; ++input) {
                        invalidate_renderer(input);
                        transparent_frame(input);
                        configure(input);
                        // release() only frees revoked, fully retired allocations.
                        if (exports->release(input)) {
                            std::scoped_lock lock(mutex);
                            if (((subscribed & (1U << input)) == 0U) && inputs.at(input).renderer_contexts.empty()) {
                                reservation->release(input);
                            }
                        }
                    }

                    {
                        std::scoped_lock lock(mutex);
                        if (closing && exports->idle()) {
                            drained = true;
                            return;
                        }
                    }

                    std::this_thread::sleep_for(1ms);
                }
            } catch (const std::exception& failure) {
                std::scoped_lock lock(mutex);
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
        if (state->exports) {
            worker = std::jthread([state = state](const std::stop_token& stop) { state->run(stop); });
        }
    }
};

media_input_session_s::media_input_session_s(gpu::device_s& device, std::shared_ptr<media_input_runtime_s> runtime)
    : impl_(std::make_unique<impl_s>(device, std::move(runtime)))
{
}

media_input_session_s::~media_input_session_s() { close(); }
void media_input_session_s::attach(int browser_id)
{
    std::scoped_lock lock(impl_->state->mutex);
    impl_->state->browser_id = browser_id;
}

void media_input_session_s::revoke_context()
{
    std::scoped_lock lock(impl_->state->mutex);
    impl_->state->revoke_locked();
}

void media_input_session_s::close()
{
    std::scoped_lock lock(impl_->state->mutex);
    if (!impl_->state->closing) {
        impl_->state->closing = true;
        impl_->state->revoke_locked();
    }
}

uint32_t media_input_session_s::demand() const
{
    std::scoped_lock lock(impl_->state->mutex);
    return impl_->state->exports && !impl_->state->closing && !impl_->state->drained && !impl_->state->bridge_failed
               ? impl_->state->subscribed
               : 0;
}

bool media_input_session_s::idle() const
{
    std::scoped_lock lock(impl_->state->mutex);
    return !impl_->state->exports || impl_->state->drained;
}

bool media_input_session_s::receive(const CefRefPtr<CefBrowser>&        browser,
                                    const CefRefPtr<CefFrame>&          frame,
                                    CefProcessId                        source,
                                    const CefRefPtr<CefProcessMessage>& message)
{
    if (!is_main_renderer_message(browser, frame, source, message)) {
        return false;
    }

    auto&            state = *impl_->state;
    std::scoped_lock lock(state.mutex);
    const auto       name    = message->GetName();
    const auto       args    = message->GetArgumentList();
    const auto       context = command_protocol::decode_context(args);
    if (name == command_protocol::CONTEXT_READY && context) {
        state.revoke_locked();
        if (!state.closing) {
            state.context = context->token;
        }

        return true;
    }

    if (name == command_protocol::CONTEXT_RELEASED && context) {
        if (state.context == context->token) {
            state.revoke_locked();
        }

        return true;
    }

    if (name == MEDIA_INPUT_RETIRED) {
        // CEF's process-level channel survives detachment of the originating frame.
        // Only remove known retirement accounting; this message never changes live demand.
        if (state.exports && args->GetSize() == 2 && args->GetType(0) == VTYPE_STRING &&
            args->GetType(1) == VTYPE_INT && args->GetInt(1) >= 0 && args->GetInt(1) < 8) {
            const auto input = static_cast<size_t>(args->GetInt(1));
            const auto token = args->GetString(0).ToString();
            // This channel is independent of current-frame activity. A drained
            // acknowledgement can arrive after the same document reactivates.
            if (token != state.context || (state.subscribed & (1U << input)) == 0U) {
                state.inputs.at(input).renderer_contexts.erase(token);
            }
        }
        return true;
    }

    if (name == MEDIA_INPUT_FAILURE) {
        if (state.exports && !state.closing && args->GetSize() == 2 && args->GetType(0) == VTYPE_STRING &&
            !state.context.empty() && args->GetString(0).ToString() == state.context &&
            args->GetType(1) == VTYPE_STRING) {
            state.error         = args->GetString(1).ToString();
            state.bridge_failed = true;
            state.revoke_locked();
        }

        return true;
    }

    if (name != MEDIA_INPUT_ACTIVITY) {
        return false;
    }

    state.receive_activity_locked(args);

    return true;
}

std::function<void()> media_input_session_s::record(size_t                input,
                                                    gpu::recording_s&     commands,
                                                    const gpu::texture_s* source,
                                                    std::string_view      source_node,
                                                    std::string_view      source_interface,
                                                    int64_t               timestamp_us)
{
    if (input >= INPUTS) {
        throw std::out_of_range("Invalid browser input index");
    }

    auto& state = *impl_->state;
    {
        std::scoped_lock lock(state.mutex);
        if (!state.exports || state.closing || state.drained || state.bridge_failed ||
            ((state.subscribed & (1U << input)) == 0U)) {
            return {};
        }

        auto& entry        = state.inputs.at(input);
        entry.timestamp_us = timestamp_us;
        const auto extent  = (source != nullptr) ? source->extent() : DISCONNECTED_EXTENT;
        if (entry.transparent != (source == nullptr) || entry.wanted != extent || entry.source_node != source_node ||
            entry.source_interface != source_interface) {
            state.exports->invalidate(input);
            entry.transparent      = (source == nullptr);
            entry.wanted           = extent;
            entry.source_node      = source_node;
            entry.source_interface = source_interface;
            entry.error.clear();
            ++entry.revision;
        }

        if ((source == nullptr) || entry.configured != entry.revision || !entry.error.empty()) {
            return {};
        }
    }

    auto publication = state.exports->record(input, commands, *source, timestamp_us);
    if (!publication) {
        return {};
    }

    return [publication, owner = impl_->state] {
        publication->commit();
        std::scoped_lock lock(owner->mutex);
        ++owner->submitted;
    };
}

media_input_metrics_s media_input_session_s::metrics() const
{
    auto&                 state = *impl_->state;
    std::scoped_lock      lock(state.mutex);
    media_input_metrics_s result{.available      = bool(state.exports),
                                 .failed         = state.exports && (state.bridge_failed || state.exports->failed() ||
                                                             (state.drained && !state.closing)),
                                 .subscribed     = state.subscribed,
                                 .submitted      = state.submitted,
                                 .delivered      = state.delivered,
                                 .drops          = state.transport_drops,
                                 .reserved_bytes = state.reservation ? state.reservation->reserved() : 0,
                                 .export_bytes   = state.exports ? state.exports->allocated_bytes() : 0,
                                 .error          = state.error};
    if (state.exports) {
        for (size_t input = 0; input < INPUTS; ++input) {
            const auto metrics = state.exports->metrics(input);
            result.drops += metrics.capacity_drops;
            result.occupied += metrics.occupied;
            if (result.error.empty() && !state.inputs.at(input).error.empty()) {
                result.error = std::format("Input {}: {}", input, state.inputs.at(input).error);
            }
        }
    }

    return result;
}
} // namespace miximus::nodes::cef::detail
