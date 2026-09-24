#include "gpu/device.hpp"
#include "gpu/tests/color_compare.hpp"
#include "include/cef_parser.h"
#include "logger/logger.hpp"
#include "nodes/cef/subsystem.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace miximus;
using namespace miximus::nodes::cef;
using namespace std::chrono_literals;

std::array<float, 4> color(size_t input, int stage)
{
    if (stage == 1 && input == 1)
        return {0, 0, 0, 1};
    const auto  bits  = ((input + 1) & 7) ^ size_t(stage == 1 ? 3 : stage == 2 ? 5 : 0);
    const float alpha = 0.5F + float(input) / 16;
    return {
        (bits & 1 ? 0.6F : 0.08F) * alpha, (bits & 2 ? 0.6F : 0.08F) * alpha, (bits & 4 ? 0.6F : 0.08F) * alpha, alpha};
}

void run(subsystem_s& subsystem, gpu::device_s& gpu)
{
    std::string page =
        R"HTML(<!doctype html><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}body{display:flex}video{width:80px;min-width:0;height:100%;object-fit:fill}</style>)HTML";
    for (int input = 0; input < 8; ++input)
        page += "<video muted autoplay playsinline></video>";
    page +=
        R"HTML(<script>document.querySelectorAll('video').forEach((v,inputIndex)=>miximus.getInputMediaStream({inputIndex}).then(s=>{v.srcObject=s;return v.play()}).catch(console.error))</script>)HTML";
    auto                       request = subsystem.create_session({
                              .url = "data:text/html," + CefURIEncode(page, false).ToString(), .dimensions = {640, 360},
                                      .frame_rate = 60
    });
    std::shared_ptr<session_s> session;
    const auto                 deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        session = request->session();
        if (session && session->media_input_demand() == 255)
            break;
        if (!request->error().empty())
            throw std::runtime_error(request->error());
        std::this_thread::sleep_for(10ms);
    }
    if (!session || session->media_input_demand() != 255)
        throw std::runtime_error("Session did not subscribe to eight inputs");
    auto                            source  = gpu.create_texture({640, 360});
    auto                            resized = gpu.create_texture({320, 180});
    auto                            context = gpu.create_recording_context(3);
    gpu::detail::color_comparison_s compare(source, MIXIMUS_CEF_COMPARE_SHADER);
    auto                            counters = gpu.create_buffer(8, gpu::host_access_e::read_write);
    for (int stage = 0; stage < 3; ++stage) {
        bool verified{};
        if (stage == 2 && !request->reload())
            throw std::runtime_error("Session reload was rejected");
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 120; ++frame) {
            auto record = context.try_record();
            if (!record)
                throw std::runtime_error("Probe recording capacity exhausted");
            for (size_t input = 0; input < 8; ++input) {
                auto& texture = input == 7 && stage == 1 ? resized : source;
                record->clear(texture, color(input, stage));
                const bool disconnected = stage == 1 && input == 1;
                auto       publish      = session->record_media_input(input,
                                                           *record,
                                                           disconnected ? nullptr : &texture,
                                                           disconnected               ? ""
                                                                      : stage == 1 && input == 0 ? "replacement"
                                                                                                 : "producer",
                                                           "tex",
                                                           int64_t(stage * 120 + frame) * 16667);
                if (publish)
                    record->on_submitted([publish = std::move(publish)](gpu::completion_s) { publish(); });
            }
            (void)record->submit();
            record.reset();
            const auto now = utils::flicks_now();
            session->advance_frames(now, now, false);
            (void)session->submit_frame(now);
            auto output = session->resolve_frame();
            session->release_prepared_frame();
            if (output && !verified) {
                std::ranges::fill(counters.writable_bytes(), std::byte{});
                auto comparison = context.try_record();
                if (comparison) {
                    for (size_t input = 0; input < 8; ++input)
                        compare.record(*comparison,
                                       output->texture(),
                                       counters,
                                       color(input, stage),
                                       0.012F,
                                       static_cast<uint32_t>(input * 80),
                                       static_cast<uint32_t>((input + 1) * 80));
                    if (comparison->submit().wait(5s) != gpu::wait_result_e::ready)
                        throw std::runtime_error("Session GPU comparison timed out");
                    uint32_t mismatches{};
                    std::memcpy(&mismatches, counters.readable_bytes().data(), sizeof(mismatches));
                    verified = mismatches == 0;
                }
            }
            const auto metrics = session->metrics();
            if (!metrics.error.empty() || !metrics.inputs.error.empty())
                throw std::runtime_error(metrics.error + " " + metrics.inputs.error);
            std::this_thread::sleep_until(start + (frame + 1) * 16667us);
        }
        if (!verified)
            throw std::runtime_error(std::format("Session stage {} did not produce its expected GPU pattern", stage));
        auto width = session->request("() => document.querySelectorAll('video')[7].videoWidth", "null");
        if (width.wait_for(5s) != std::future_status::ready)
            throw std::runtime_error("Video dimension metadata timed out");
        const auto result = width.get();
        if (!result.error.empty() || result.json != (stage == 1 ? "320" : "640"))
            throw std::runtime_error("Video input resize metadata mismatch: " + result.json + result.error);
        const auto m = session->metrics().inputs;
        std::cout << "Stage " << stage << ": submitted=" << m.submitted << " delivered=" << m.delivered
                  << " drops=" << m.drops << " held=" << m.occupied << " reserved=" << m.reserved_bytes << '\n';
    }
    session.reset();
    request.reset();
}
} // namespace
int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "Usage: cef_media_input_session_probe RUNTIME PROFILE\n";
        return 2;
    }
    try {
        std::cout.setf(std::ios::unitbuf);
        logger::init_loggers(spdlog::level::warn);
        gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation            = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        options.external_image_import = true;
        gpu::device_s gpu(options);
        {
            subsystem_s subsystem(gpu, argv[2], argv[1]);
            run(subsystem, gpu);
        }
        if (gpu.validation_errors())
            throw std::runtime_error("Vulkan validation errors");
        std::cout << "Session inputs passed: eight streams, source replacement, disconnect, resize, reload, shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
