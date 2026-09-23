#include "gpu/device.hpp"
#include "logger/logger.hpp"
#include "nodes/cef/detail/browser_session.hpp"
#include "nodes/cef/detail/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::nodes::cef::detail;
using namespace std::chrono_literals;

// Probe-only blocking cleanup. Real nodes request closure and let their app-owned
// subsystem retain sessions until callbacks and resources have drained.
struct scoped_session_s
{
    browser_session_s session;
    scoped_session_s(gpu::device_s& device, browser_session_s::options_s options)
        : session(device, std::move(options))
    {
    }
    ~scoped_session_s()
    {
        session.close_async();
        session.reset_frames();
        if (!session.wait_closed(10s))
            std::terminate();
    }

    void finish()
    {
        session.close_async();
        session.reset_frames();
        if (!session.wait_closed(10s))
            throw std::runtime_error("Browser did not close");
        const auto result = session.metrics();
        if (!result.error.empty())
            throw std::runtime_error(result.error);
    }
};

browser_session_s::frame_ptr_t consume(gpu::device_s& device, browser_session_s& session, bool animated)
{
    auto                           context     = device.create_recording_context(3);
    auto                           destination = device.create_texture({640, 360});
    const auto                     start       = std::chrono::steady_clock::now();
    auto                           next        = start;
    gpu::completion_s              completion;
    browser_session_s::frame_ptr_t retained;
    uint64_t                       consumed{};
    while (std::chrono::steady_clock::now() - start < 15s) {
        const auto now = utils::flicks_now();
        session.advance_frames(now, now, false);
        if (session.submit_frame(now)) {
            const auto frame     = session.resolve_frame();
            auto       recording = frame ? context.try_record() : nullptr;
            if (recording) {
                gpu::draw_s draw;
                draw.compositing = gpu::compositing_e::replace;
                recording->draw(frame->texture(), destination, draw);
                completion = recording->submit();
                retained   = frame;
                ++consumed;
            }
        }
        session.release_prepared_frame();
        const auto metrics = session.metrics();
        if (!metrics.error.empty())
            throw std::runtime_error(metrics.error);
        const bool enough = animated ? metrics.copied >= 120 && consumed >= 90
                                     : metrics.copied > 0 && consumed >= 60 && metrics.source_queue.repeated > 0;
        if (enough) {
            if (completion.wait(5s) != gpu::wait_result_e::ready)
                throw std::runtime_error("Session consumer GPU work did not complete");
            std::cout << (animated ? "Animated" : "Static") << " session: captured=" << metrics.copied
                      << " consumed=" << consumed << " repeated=" << metrics.source_queue.repeated
                      << " dropped=" << metrics.dropped << '\n';
            return retained;
        }
        next += 16667us;
        std::this_thread::sleep_until(next);
    }
    throw std::runtime_error("Timed session consumption did not reach its target");
}

void exhaust_and_recover(browser_session_s& session)
{
    std::vector<browser_session_s::frame_ptr_t> held;
    const auto                                  deadline = std::chrono::steady_clock::now() + 5s;
    while (held.size() < 8 && std::chrono::steady_clock::now() < deadline) {
        const auto now = utils::flicks_now();
        session.advance_frames(now, now, false);
        if (session.submit_frame(now)) {
            auto frame = session.resolve_frame();
            if (frame && std::find(held.begin(), held.end(), frame) == held.end())
                held.push_back(std::move(frame));
        }
        session.release_prepared_frame();
        std::this_thread::sleep_for(10ms);
    }
    if (held.size() != 8)
        throw std::runtime_error("Could not retain all session pool slots");
    const auto before = session.metrics();
    std::this_thread::sleep_for(150ms);
    const auto exhausted = session.metrics();
    if (exhausted.dropped <= before.dropped)
        throw std::runtime_error("Session did not drop paints when its pool was exhausted");
    held.clear();
    const auto recovery_deadline = std::chrono::steady_clock::now() + 2s;
    while (session.metrics().copied == exhausted.copied && std::chrono::steady_clock::now() < recovery_deadline)
        std::this_thread::sleep_for(10ms);
    const auto recovered = session.metrics();
    if (recovered.copied == exhausted.copied || !recovered.error.empty())
        throw std::runtime_error("Session failed to recover released pool capacity");
    std::cout << "Pool exhaustion dropped " << exhausted.dropped - before.dropped
              << " paints and recovered after leases were released\n";
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
        return 2;
    std::cout.setf(std::ios::unitbuf);
    logger::init_loggers(spdlog::level::warn);
    try {
        gpu::device_options_s options;
        options.external_image_import = true;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        gpu::device_s device(options);
        {
            runtime_s runtime(argv[1], argv[2]);
            {
                scoped_session_s early(device,
                                       {
                                           .url        = "about:blank",
                                           .dimensions = {640, 360},
                });
                early.session.start_async();
                early.session.close_async();
                early.finish();
            }
            std::cout << "Pending browser creation closed successfully\n";
            browser_session_s::frame_ptr_t old_generation;
            {
                scoped_session_s animated(
                    device,
                    {
                        .url        = "data:text/html,<html><style>@keyframes move{from{transform:translateX(0px)}"
                                      "to{transform:translateX(220px)}}</style><body style='background:transparent'>"
                                      "<div style='background:rgba(128,64,32,0.5);width:200px;height:200px;"
                                      "animation:move 1s linear infinite alternate'></div></body></html>",
                        .dimensions = {640, 360},
                });
                animated.session.start_async();
                old_generation = consume(device, animated.session, true);
                old_generation.reset();
                exhaust_and_recover(animated.session);
                old_generation = consume(device, animated.session, true);
                animated.finish();
            }
            {
                scoped_session_s still(
                    device,
                    {
                        .url = "data:text/html,<body style='background:rgba(128,64,32,0.5)'>Static source</body>",
                        .dimensions = {800, 450},
                });
                still.session.start_async();
                consume(device, still.session, false);
                still.finish();
            }
            auto context   = device.create_recording_context(1);
            auto target    = device.create_texture({640, 360});
            auto recording = context.try_record();
            if (!old_generation || !recording)
                throw std::runtime_error("Old session frame was not retained");
            recording->draw(old_generation->texture(), target, {});
            if (recording->submit().wait(5s) != gpu::wait_result_e::ready)
                throw std::runtime_error("Retained session frame could not be consumed after browser closure");
            std::cout << "Old viewport generation remains a usable ordinary GPU texture after closure\n";
        }
        return device.validation_errors() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "CEF session probe failed: " << error.what() << '\n';
        return 1;
    }
}
