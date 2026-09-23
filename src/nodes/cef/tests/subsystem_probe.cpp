#include "logger/logger.hpp"
#include "nodes/cef/subsystem.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
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
    const auto navigation = command_result(session.request(
        "() => { setTimeout(() => location.href='about:blank', 0); return new Promise(() => {}); }", "null"));
    if (navigation.error.empty() || navigation.error.find("timed out") != std::string::npos)
        throw std::runtime_error("Navigation did not cancel its pending request");
    await_context(session);
    if (command_result(session.request("() => location.href", "null")).json != R"("about:blank")")
        throw std::runtime_error("New navigation context did not accept commands");
    std::cout
        << "JSON/Promise replies, exceptions, bounded pending requests, timeouts and navigation cancellation passed\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    try {
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
                .url = "data:text/html,<body style='background:blue'>Resize</body>", .dimensions = {800, 450}
            });
            session = await_session(*request);
            frame   = await_frame(*session);
            if (frame->texture().dimensions() != gpu::vec2i_t{800, 450})
                throw std::runtime_error("Replacement viewport was not applied");
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
