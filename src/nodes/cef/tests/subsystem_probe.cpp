#include "logger/logger.hpp"
#include "nodes/cef/subsystem.hpp"

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
            session.reset();
            const auto start = std::chrono::steady_clock::now();
            request.reset();
            if (std::chrono::steady_clock::now() - start > 100ms)
                throw std::runtime_error("Node removal waited for retirement");
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
