#include "gpu/tests/color_compare.hpp"
#include "include/cef_browser.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_task.h"
#include "logger/logger.hpp"
#include "nodes/cef/subsystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::nodes::cef;
using namespace std::chrono_literals;

auto await_session(session_request_s& request)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto session = request.session())
            return session;
        if (const auto error = request.error(); !error.empty())
            throw std::runtime_error(error);
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Session creation timed out");
}

auto await_frame(detail::browser_session_s& session)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = utils::flicks_now();
        session.advance_frames(now, now, false);
        (void)session.submit_frame(now);
        auto frame = session.resolve_frame();
        session.release_prepared_frame();
        if (frame)
            return frame;
        if (const auto error = session.metrics().error; !error.empty())
            throw std::runtime_error(error);
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Session capture timed out");
}
void await_context(detail::browser_session_s& session)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!session.context_ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    if (!session.context_ready())
        throw std::runtime_error("Renderer context was not announced");
}

auto command_result(std::future<detail::browser_session_s::command_result_s> result)
{
    if (result.wait_for(5s) != std::future_status::ready)
        throw std::runtime_error("Command future did not settle");
    return result.get();
}

void exercise_commands(detail::browser_session_s& session)
{
    await_context(session);
    const auto dialogs = command_result(session.request(
        "() => { alert('suppressed'); return {confirm:confirm('suppressed'), prompt:prompt('suppressed','default')}; }",
        "null"));
    if (!dialogs.error.empty() || nlohmann::json::parse(dialogs.json) != nlohmann::json{
                                                                             {"confirm", false  },
                                                                             {"prompt",  nullptr}
    })
        throw std::runtime_error("JavaScript dialogs were not suppressed");
    const auto popup = command_result(session.request("() => window.open('about:blank','_blank') === null", "null"));
    if (!popup.error.empty() || popup.json != "true")
        throw std::runtime_error("New browser window was not denied");
    std::cout << "JavaScript dialogs suppressed and window.open denied\n";
    auto       first  = session.request("value => new Promise(resolve => setTimeout(() => resolve({echo:value}), 50))",
                                 R"({"message":"hello","number":42})");
    const auto second = command_result(session.request("value => value + 1", "4"));
    const auto first_result = command_result(std::move(first));
    if (!first_result.error.empty() || nlohmann::json::parse(first_result.json)["echo"]["number"] != 42 ||
        second.json != "5")
        throw std::runtime_error("JSON/Promise result correlation failed");
    for (const auto* source : {"() => { throw new Error('expected exception'); }",
                               "() => Promise.reject('expected rejection')",
                               "() => undefined",
                               "() => { const x={}; x.self=x; return x; }"}) {
        if (command_result(session.request(source, "null")).error.empty())
            throw std::runtime_error("JavaScript failure was not reported");
    }
    if (command_result(session.request("x => x", "not json")).error.empty())
        throw std::runtime_error("Invalid request JSON was accepted");
    std::vector<std::future<detail::browser_session_s::command_result_s>> pending;
    for (int index = 0; index < 64; ++index)
        pending.push_back(session.request("() => new Promise(() => {})", "null", 500ms));
    if (command_result(session.request("() => 1", "null")).error.empty())
        throw std::runtime_error("Native pending command limit was not enforced");
    for (auto& result : pending)
        if (command_result(std::move(result)).error.empty())
            throw std::runtime_error("Unresolved Promise did not time out");
    const auto recovery_deadline = std::chrono::steady_clock::now() + 2s;
    bool       recovered{};
    while (!recovered && std::chrono::steady_clock::now() < recovery_deadline) {
        recovered = command_result(session.request("() => 'after timeout'", "null")).json == R"("after timeout")";
        if (!recovered)
            std::this_thread::sleep_for(10ms);
    }
    if (!recovered)
        throw std::runtime_error("Renderer command capacity did not recover after cancellation acknowledgements");
    const auto installed = command_result(session.set_program_time_handler(
        "time => { if (!window.testProgramTimes) window.testProgramTimes=[]; window.testProgramTimes.push(time); }"));
    if (!installed.error.empty())
        throw std::runtime_error("Cannot install internal program-time handler: " + installed.error);
    // Adjacent recovered program frames must both arrive, including exact
    // integer metadata beyond JavaScript's safe Number range.
    constexpr uint64_t frame_number = 9'007'199'254'740'992ULL;
    for (uint64_t index = 0; index < 3; ++index) {
        session.send_program_time({.frame_number   = frame_number + index,
                                   .epoch          = 7,
                                   .program_pts    = utils::flicks(11771760 * index),
                                   .frame_duration = utils::flicks(11771760),
                                   .discontinuity  = index == 0});
    }
    const auto time_reply = command_result(session.request("() => window.testProgramTimes", "null"));
    if (!time_reply.error.empty())
        throw std::runtime_error(time_reply.error);
    const auto times = nlohmann::json::parse(time_reply.json);
    if (times.size() != 3 || times[0]["frameNumber"] != "9007199254740992" ||
        times[1]["frameNumber"] != "9007199254740993" || times[2]["pts"] != "23543520" ||
        times[0]["timebase"] != "705600000" || times[0]["discontinuity"] != true ||
        session.metrics().timing_rejections != 0)
        throw std::runtime_error("Program time was coalesced, rounded or rejected");
    std::cout << "Cooperative program-time delivery preserves adjacent frames and exact integer metadata\n";
    const auto navigation = command_result(
        session.request("() => { window.onbeforeunload = e => { e.preventDefault(); e.returnValue='stay'; }; "
                        "setTimeout(() => location.href='about:blank', 0); return new Promise(() => {}); }",
                        "null"));
    if (navigation.error.empty() || navigation.error.find("timed out") != std::string::npos)
        throw std::runtime_error("Navigation did not cancel its pending request");
    await_context(session);
    if (command_result(session.request("() => location.href", "null")).json != R"("about:blank")")
        throw std::runtime_error("New navigation context did not accept commands");
    std::cout
        << "JSON/Promise replies, exceptions, bounded pending requests, timeouts and navigation cancellation passed\n";
}

// Test-process-only fault injection through CEF's public DevTools interface.
// No debugging port, production command or session API is added.
class crash_observer_s final : public CefDevToolsMessageObserver
{
    IMPLEMENT_REFCOUNTING(crash_observer_s);

  public:
    bool OnDevToolsMessage(CefRefPtr<CefBrowser>, const void* message, size_t size) override
    {
        std::cerr << "Crash probe DevTools: " << std::string_view(static_cast<const char*>(message), size) << '\n';
        return false;
    }
};

class crash_task_s final : public CefTask
{
    std::promise<CefRefPtr<CefRegistration>> result_;
    std::string                              url_;
    std::string                              method_;
    IMPLEMENT_REFCOUNTING(crash_task_s);

  public:
    crash_task_s(std::promise<CefRefPtr<CefRegistration>> result, std::string url, std::string method)
        : result_(std::move(result))
        , url_(std::move(url))
        , method_(std::move(method))
    {
    }
    void Execute() override
    {
        // This probe creates fewer than 32 browsers, in its own embedded runtime.
        // Match the fixture URL as well; never target another live test browser.
        for (int id = 1; id <= 32; ++id) {
            auto browser = CefBrowserHost::GetBrowserByIdentifier(id);
            if (browser && browser->GetMainFrame()->GetURL().ToString() == url_) {
                auto observer = browser->GetHost()->AddDevToolsMessageObserver(new crash_observer_s);
                if (browser->GetHost()->ExecuteDevToolsMethod(0, method_, nullptr) == 0)
                    observer = nullptr;
                result_.set_value(observer);
                return;
            }
        }
        result_.set_value(nullptr);
    }
};

void exercise_renderer_failure(detail::browser_session_s& session)
{
    await_context(session);
    auto                                     pending = session.request("() => new Promise(() => {})", "null");
    std::promise<CefRefPtr<CefRegistration>> injected;
    auto                                     result = injected.get_future();
    if (!CefPostTask(TID_UI,
                     new crash_task_s(std::move(injected),
                                      "data:text/html,<body style='background:blue'>Resize</body>",
                                      "Page.crash")) ||
        result.wait_for(5s) != std::future_status::ready)
        throw std::runtime_error("Cannot inject renderer crash");
    auto observer = result.get();
    if (!observer)
        throw std::runtime_error("Cannot observe renderer crash");
    const auto deadline = std::chrono::steady_clock::now() + 45s;
    while (session.metrics().phase != detail::browser_session_s::phase_e::failed &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    const auto metrics = session.metrics();
    if (metrics.phase != detail::browser_session_s::phase_e::failed ||
        metrics.error.find("renderer terminated") == std::string::npos)
        throw std::runtime_error("Renderer termination did not fail its session: phase=" +
                                 std::to_string(static_cast<int>(metrics.phase)) + " error=" + metrics.error);
    const auto cancelled = command_result(std::move(pending));
    if (cancelled.error.empty() || cancelled.error.find("timed out") != std::string::npos || session.context_ready())
        throw std::runtime_error("Renderer termination did not invalidate pending commands/context");
    std::cout << "Renderer crash reported and pending commands cancelled: " << metrics.error << '\n';
}

void exercise_gpu_failure(gpu::device_s& device, detail::browser_session_s& session)
{
    await_context(session);
    auto                                     retained = await_frame(session);
    std::promise<CefRefPtr<CefRegistration>> injected;
    auto                                     result = injected.get_future();
    if (!CefPostTask(TID_UI,
                     new crash_task_s(std::move(injected),
                                      "data:text/html,<body style='background:green'>Recovered</body>",
                                      "Browser.crashGpuProcess")) ||
        result.wait_for(5s) != std::future_status::ready)
        throw std::runtime_error("Cannot inject GPU subprocess crash");
    auto observer = result.get();
    if (!observer)
        throw std::runtime_error("Cannot observe GPU subprocess crash");
    // Test-only settling interval. Production never guesses readiness from time.
    std::this_thread::sleep_for(1s);
    auto context     = device.create_recording_context(1);
    auto destination = device.create_texture({801, 451});
    auto recording   = context.try_record();
    recording->draw(retained->texture(), destination, {});
    if (recording->submit().wait(5s) != gpu::wait_result_e::ready)
        throw std::runtime_error("Owned frame became unusable after CEF GPU subprocess loss");
    recording.reset();
    const auto before = session.metrics().copied;
    const auto changed =
        command_result(session.request("() => { document.body.style.background='red'; return null; }", "null"));
    if (!changed.error.empty())
        throw std::runtime_error("Renderer unavailable after GPU subprocess loss: " + changed.error);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (session.metrics().copied <= before && std::chrono::steady_clock::now() < deadline) {
        (void)await_frame(session);
        std::this_thread::sleep_for(10ms);
    }
    if (session.metrics().copied <= before)
        throw std::runtime_error("Accelerated capture did not resume after GPU subprocess loss");
    std::cout << "Owned frame survived CEF GPU subprocess loss and accelerated capture resumed\n";
}

void exercise_color(gpu::device_s& device, detail::browser_session_s& session)
{
    await_context(session);
    auto                            frame = await_frame(session);
    gpu::detail::color_comparison_s compare(frame->texture(), MIXIMUS_CEF_COMPARE_SHADER);
    auto                            context  = device.create_recording_context(1);
    auto                            counters = device.create_buffer(8, gpu::host_access_e::read_write);
    const auto                      decode   = [](float value) {
        return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    };
    struct case_s
    {
        const char*          css;
        std::array<float, 4> reference;
        float                tolerance;
    };
    const std::array cases{
        case_s{"rgb(255,0,0)",        {1, 0, 0, 1},                                                         0.00004F},
        case_s{"rgb(0,255,0)",        {0, 1, 0, 1},                                                         0.00004F},
        case_s{"rgb(0,0,255)",        {0, 0, 1, 1},                                                         0.00004F},
        case_s{"rgb(128,64,32)",      {decode(128.F / 255), decode(64.F / 255), decode(32.F / 255), 1},     0.00004F},
        case_s{"rgba(128,64,32,0.5)",
               {decode(.5F) * 128 / 255, decode(.25F) * 128 / 255, decode(.125F) * 128 / 255, 128.F / 255},
               0.003F                                                                                               },
        case_s{"rgba(255,0,0,0.01)",  {3.F / 255, 0, 0, 3.F / 255},                                         0.004F  },
        case_s{"transparent",         {0, 0, 0, 0},                                                         0.00004F},
    };
    for (const auto& test : cases) {
        const auto result = command_result(session.request(
            "color => { "
            "document.documentElement.style.cssText='margin:0;width:100%;height:100%;background:transparent';"
            "document.body.innerHTML=''; "
            "document.body.style.cssText='margin:0;width:100%;height:100%;background:'+color; return null; }",
            nlohmann::json(test.css).dump()));
        // A reply acknowledges execution, not paint. Compare eventual output.
        if (!result.error.empty())
            throw std::runtime_error(result.error);
        const auto deadline   = std::chrono::steady_clock::now() + 3s;
        uint32_t   mismatches = UINT32_MAX;
        float      maximum{};
        while (std::chrono::steady_clock::now() < deadline) {
            frame      = await_frame(session);
            auto bytes = counters.writable_bytes();
            std::fill(bytes.begin(), bytes.end(), std::byte{});
            auto recording = context.try_record();
            if (!recording)
                throw std::runtime_error("Color comparison recording unavailable");
            compare.record(*recording, frame->texture(), counters, test.reference, test.tolerance);
            const auto completed = recording->submit();
            recording.reset();
            if (completed.wait(5s) != gpu::wait_result_e::ready)
                throw std::runtime_error("Color comparison GPU work did not complete");
            const auto              statistics = counters.readable_bytes();
            std::array<uint32_t, 2> values{};
            std::memcpy(values.data(), statistics.data(), sizeof(values));
            mismatches = values[0];
            maximum    = std::bit_cast<float>(values[1]);
            if (mismatches == 0)
                break;
            std::this_thread::sleep_for(20ms);
        }
        std::cout << "GPU color comparison " << test.css << ": mismatches=" << mismatches << " max_error=" << maximum
                  << '\n';
        if (mismatches != 0)
            throw std::runtime_error("CEF GPU color/alpha comparison failed");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    try {
        // This executable intentionally crashes its own renderer. Avoid lengthy
        // systemd core collection delaying the child-exit notification. The
        // limit applies only to this probe and its children, never the app/host.
        const rlimit core_limit{0, 0};
        if (setrlimit(RLIMIT_CORE, &core_limit) != 0)
            throw std::runtime_error("Cannot disable core dumps for the crash probe");
        logger::init_loggers(spdlog::level::info);
        gpu::device_options_s options;
        options.external_image_import = true;
        options.validation            = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        gpu::device_s device(options);
        {
            subsystem_s                                     subsystem(device, argv[2], argv[1]);
            std::vector<std::unique_ptr<session_request_s>> pending;
            for (int index = 0; index < 16; ++index)
                pending.push_back(subsystem.create_session({
                    .url = "about:blank", .dimensions = {32, 32}
                }));
            auto excess = subsystem.create_session({
                .url = "about:blank", .dimensions = {32, 32}
            });
            if (excess->error().empty())
                throw std::runtime_error("Session count limit was not enforced");
            pending.clear();
            excess.reset();
            // Retry admission while the accepted cancellation tasks drain.
            std::unique_ptr<session_request_s> request;
            const auto                         deadline = std::chrono::steady_clock::now() + 10s;
            do {
                request = subsystem.create_session({
                    .url = "data:text/html,<body style='background:red'>CEF</body>", .dimensions = {640, 360}
                });
                if (request->error().empty())
                    break;
                std::this_thread::sleep_for(10ms);
            } while (std::chrono::steady_clock::now() < deadline);
            auto session = await_session(*request);
            auto frame   = await_frame(*session);
            exercise_commands(*session);
            exercise_color(device, *session);
            auto closing_command = session->request("() => new Promise(() => {})", "null");
            std::this_thread::sleep_for(20ms);
            session.reset();
            const auto start = std::chrono::steady_clock::now();
            request.reset();
            if (std::chrono::steady_clock::now() - start > 100ms)
                throw std::runtime_error("Node removal waited for retirement");
            const auto cancelled = command_result(std::move(closing_command));
            if (cancelled.error.empty() || cancelled.error.find("timed out") != std::string::npos)
                throw std::runtime_error("Browser closure did not cancel its pending command");
            auto context   = device.create_recording_context(1);
            auto output    = device.create_texture({640, 360});
            auto recording = context.try_record();
            recording->draw(frame->texture(), output, {});
            frame.reset();
            std::this_thread::sleep_for(50ms);
            // Retirement must retain GPU resources even with no frame lease.
            if (recording->submit().wait(5s) != gpu::wait_result_e::ready)
                throw std::runtime_error("Retiring generation GPU use failed");
            recording.reset();
            request = subsystem.create_session({
                .url = "data:text/html,<body style='background:blue'>Resize</body>", .dimensions = {801, 451}
            });
            session = await_session(*request);
            frame   = await_frame(*session);
            if (frame->texture().dimensions() != gpu::vec2i_t{801, 451})
                throw std::runtime_error("Replacement viewport was not applied");
            exercise_renderer_failure(*session);
            frame.reset();
            session.reset();
            request.reset();
            request = subsystem.create_session({
                .url = "data:text/html,<body style='background:green'>Recovered</body>", .dimensions = {801, 451}
            });
            session = await_session(*request);
            frame   = await_frame(*session);
            if (frame->texture().dimensions() != gpu::vec2i_t{801, 451})
                throw std::runtime_error("Odd-sized replacement after renderer crash failed");
            std::cout << "Fresh renderer captured an odd-sized viewport after crash retirement\n";
            exercise_gpu_failure(device, *session);
            auto invalid = subsystem.create_session({
                .url = "about:blank", .dimensions = {8192, 8192}
            });
            if (invalid->error().empty())
                throw std::runtime_error("Texture budget was not enforced");
            std::cout << "Bounded admission, pending cancellation, asynchronous removal, GPU retirement and "
                         "replacement passed\n";
        }
        return device.validation_errors() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "CEF subsystem probe failed: " << error.what() << '\n';
        return 1;
    }
}
