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
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace miximus;
using namespace miximus::nodes::cef;
using namespace std::chrono_literals;

std::array<float, 4> color(size_t input, int stage)
{
    if (stage < 0 || (stage == 1 && input == 1)) {
        return {0, 0, 0, 0};
    }

    size_t mask{};
    if (stage == 1) {
        mask = 3;
    } else if (stage == 2) {
        mask = 5;
    }

    const auto  bits  = ((input + 1) & 7) ^ mask;
    const float alpha = 0.5F + (float(input) / 16);
    return {(((bits & 1) != 0U) ? 0.6F : 0.08F) * alpha,
            (((bits & 2) != 0U) ? 0.6F : 0.08F) * alpha,
            (((bits & 4) != 0U) ? 0.6F : 0.08F) * alpha,
            alpha};
}

class frame_probe_s
{
    std::shared_ptr<session_s>      session_;
    std::array<gpu::texture_s, 8>   sources_;
    gpu::texture_s                  resized_;
    gpu::recording_context_s        context_;
    gpu::detail::color_comparison_s compare_;
    gpu::buffer_s                   counters_;

  public:
    frame_probe_s(gpu::device_s& gpu, std::shared_ptr<session_s> browser_session, bool small_inputs)
        : session_(std::move(browser_session))
        , sources_(create_sources(gpu, small_inputs))
        , resized_(gpu.create_texture(small_inputs ? gpu::extent_s{.width = 63, .height = 7}
                                                   : gpu::extent_s{.width = 320, .height = 180}))
        , context_(gpu.create_recording_context(3))
        , compare_(sources_.at(0), MIXIMUS_CEF_COMPARE_SHADER)
        , counters_(gpu.create_buffer(8, gpu::host_access_e::read_write))
    {
    }

  private:
    static std::array<gpu::texture_s, 8> create_sources(gpu::device_s& gpu, bool small_inputs)
    {
        constexpr std::array<gpu::extent_s, 8> small_sizes{
            {{.width = 1, .height = 1},
             {.width = 7, .height = 3},
             {.width = 8, .height = 8},
             {.width = 16, .height = 16},
             {.width = 32, .height = 32},
             {.width = 64, .height = 64},
             {.width = 128, .height = 128},
             {.width = 129, .height = 127}}
        };
        std::array<gpu::texture_s, 8> textures;
        for (size_t input = 0; input < textures.size(); ++input) {
            textures.at(input) =
                gpu.create_texture(small_inputs ? small_sizes.at(input) : gpu::extent_s{.width = 640, .height = 360});
        }

        return textures;
    }

    void record_inputs(int stage, int frame)
    {
        auto record = context_.try_record();
        if (!record) {
            throw std::runtime_error("Probe recording capacity exhausted");
        }

        for (size_t input = 0; input < 8; ++input) {
            auto& texture = input == 7 && stage == 1 ? resized_ : sources_.at(input);
            record->clear(texture, color(input, stage));
            const bool       disconnected = stage < 0 || (stage == 1 && input == 1);
            std::string_view producer     = stage == 1 && input == 0 ? "replacement" : "producer";
            if (disconnected) {
                producer = "";
            }

            auto publish = session_->record_media_input(input,
                                                        *record,
                                                        disconnected ? nullptr : &texture,
                                                        producer,
                                                        "tex",
                                                        int64_t(((stage + 1) * 120) + frame) * 16667);
            if (publish) {
                record->on_submitted([publish = std::move(publish)](const gpu::completion_s&) { publish(); });
            }
        }

        (void)record->submit();
        record.reset();
    }

    bool verify_output(const gpu::texture_s& output, int stage)
    {
        std::ranges::fill(counters_.writable_bytes(), std::byte{});
        auto comparison = context_.try_record();
        if (comparison) {
            for (size_t input = 0; input < 8; ++input) {
                compare_.record(*comparison,
                                output,
                                counters_,
                                color(input, stage),
                                0.012F,
                                static_cast<uint32_t>(input * 80),
                                static_cast<uint32_t>((input + 1) * 80));
            }

            if (comparison->submit().wait(5s) != gpu::wait_result_e::ready) {
                throw std::runtime_error("Session GPU comparison timed out");
            }

            uint32_t mismatches{};
            std::memcpy(&mismatches, counters_.readable_bytes().data(), sizeof(mismatches));
            return mismatches == 0;
        }

        return false;
    }

    void check_dimensions(int stage)
    {
        auto dimensions = session_->request(
            "() => [...document.querySelectorAll('video')].map(v => [v.videoWidth, v.videoHeight])", "null");
        if (dimensions.wait_for(5s) != std::future_status::ready) {
            throw std::runtime_error("Video dimension metadata timed out");
        }

        std::string expected = "[";
        for (size_t input = 0; input < sources_.size(); ++input) {
            auto extent = stage == 1 && input == 7 ? resized_.extent() : sources_.at(input).extent();
            if (stage < 0 || (stage == 1 && input == 1)) {
                extent = {.width = 16, .height = 16};
            }
            expected += std::format("{}[{},{}]", (input != 0U) ? "," : "", extent.width, extent.height);
        }

        expected += "]";
        const auto result = dimensions.get();
        if (!result.error.empty() || result.json != expected) {
            throw std::runtime_error("Video input dimension metadata mismatch: " + result.json + result.error);
        }
    }

  public:
    void run_stage(int stage)
    {
        bool       verified{};
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 120; ++frame) {
            record_inputs(stage, frame);
            const auto now = utils::flicks_now();
            session_->advance_frames(now, now, false);
            (void)session_->submit_frame(now);
            auto output = session_->resolve_frame();
            session_->release_prepared_frame();
            if (output && !verified) {
                verified = verify_output(output->texture(), stage);
            }

            const auto metrics = session_->metrics();
            if (!metrics.error.empty() || !metrics.inputs.error.empty()) {
                throw std::runtime_error(metrics.error + " " + metrics.inputs.error);
            }

            std::this_thread::sleep_until(start + (frame + 1) * 16667us);
        }

        if (!verified) {
            throw std::runtime_error(std::format("Session stage {} did not produce its expected GPU pattern", stage));
        }

        check_dimensions(stage);

        const auto m = session_->metrics().inputs;
        if (stage < 0 && ((m.submitted != 0U) || (m.export_bytes != 0U) || (m.reserved_bytes != 0U))) {
            throw std::runtime_error("Disconnected streams allocated or exported dummy GPU frames");
        }

        std::cout << "Stage " << stage << ": submitted=" << m.submitted << " delivered=" << m.delivered
                  << " drops=" << m.drops << " held=" << m.occupied << " reserved=" << m.reserved_bytes << '\n';
    }
};

void run_script(const std::shared_ptr<session_s>& session, const std::string& body)
{
    auto result = session->request("async () => { await (" + body + ")(null); return true; }", "null");
    if (result.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("Lifecycle script timed out");
    }

    const auto reply = result.get();
    if (!reply.error.empty()) {
        throw std::runtime_error(reply.error);
    }
}

void wait_demand(const std::shared_ptr<session_s>& session, uint32_t wanted, bool released)
{
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 5s) {
        const auto now = utils::flicks_now();
        session->advance_frames(now, now, false);
        (void)session->submit_frame(now);
        session->release_prepared_frame();
        const auto m = session->metrics().inputs;
        if (session->media_input_demand() == wanted &&
            (!released || ((m.export_bytes == 0U) && (m.occupied == 0U) && (m.reserved_bytes == 0U)))) {
            std::cout << "Demand " << wanted << " after "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                             .count()
                      << "ms, export bytes=" << m.export_bytes << ", reserved bytes=" << m.reserved_bytes << '\n';
            return;
        }

        std::this_thread::sleep_for(5ms);
    }

    const auto m = session->metrics().inputs;
    throw std::runtime_error(
        std::format("Track demand/resources did not retire: demand={}, exports={}, held={}, reserved={}",
                    session->media_input_demand(),
                    m.export_bytes,
                    m.occupied,
                    m.reserved_bytes));
}

void check_budget_recovery(gpu::device_s& gpu, const std::shared_ptr<session_s>& session)
{
    auto       source  = gpu.create_texture({4096, 4096});
    auto       context = gpu.create_recording_context(3);
    int64_t    timestamp{};
    const auto feed = [&] {
        for (int frame = 0; frame < 180; ++frame) {
            timestamp += 16667;
            if (auto record = context.try_record()) {
                record->clear(source, {0.5F, 0, 0, 1});
                for (size_t input = 0; input < 8; ++input) {
                    auto publish = session->record_media_input(input, *record, &source, "producer", "tex", timestamp);
                    if (publish) {
                        record->on_submitted([publish](const gpu::completion_s&) { publish(); });
                    }
                }
                (void)record->submit();
            }
            std::this_thread::sleep_for(16667us);
        }
    };
    feed();
    const auto       initial          = session->metrics().inputs;
    constexpr size_t two_input_charge = 2ULL * 4096 * 4096 * 4 * 10;
    if (initial.reserved_bytes != two_input_charge || initial.delivered == 0 || initial.error.empty()) {
        throw std::runtime_error("Failed allocations retained admission or did not exercise capacity exhaustion");
    }
    for (int stopped = 2; stopped <= 8; stopped += 2) {
        run_script(session,
                   std::format("() => {{ [...document.querySelectorAll('video')].slice(0, {}).forEach(v => "
                               "v.srcObject.getTracks().forEach(t => t.stop())); }}",
                               stopped));
        wait_demand(session, 255U & ~((1U << stopped) - 1U), stopped == 8);
        if (stopped != 8) {
            const auto before = session->metrics().inputs.delivered;
            feed();
            const auto after = session->metrics().inputs;
            if (after.delivered <= before || after.reserved_bytes != two_input_charge) {
                throw std::runtime_error("Demanded inputs did not recover after export capacity was released");
            }
        }
    }
    std::cout << "Budget exhaustion rolled back admission and recovered all waiting inputs\n";
}

void check_shared_budget_recovery(subsystem_s& subsystem, gpu::device_s& gpu)
{
    const std::string          page = R"HTML(
<video muted autoplay></video>
<script>
  miximus.getInputMediaStream({ inputIndex: 0 }).then((stream) => {
    document.querySelector("video").srcObject = stream;
  });
</script>
)HTML";
    const session_s::options_s options{
        .url        = "data:text/html," + CefURIEncode(page, false).ToString(),
        .dimensions = {64, 64},
        .frame_rate = 60,
    };

    std::array<std::unique_ptr<session_request_s>, 4> requests;
    std::array<std::shared_ptr<session_s>, 4>         sessions;
    for (size_t index = 0; index < sessions.size(); ++index) {
        requests.at(index)  = subsystem.create_session(options);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!sessions.at(index) && std::chrono::steady_clock::now() < deadline) {
            sessions.at(index) = requests.at(index)->session();
            if (!requests.at(index)->error().empty()) {
                throw std::runtime_error(requests.at(index)->error());
            }

            std::this_thread::sleep_for(10ms);
        }
        if (!sessions.at(index)) {
            throw std::runtime_error("Shared-budget session creation timed out");
        }

        wait_demand(sessions.at(index), 1, false);
    }
    auto       source  = gpu.create_texture({4096, 4096});
    auto       context = gpu.create_recording_context(3);
    int64_t    timestamp{};
    const auto feed = [&] {
        for (int frame = 0; frame < 180; ++frame) {
            timestamp += 16667;
            if (auto record = context.try_record()) {
                record->clear(source, {0.5F, 0, 0, 1});
                for (const auto& session : sessions) {
                    auto publish = session->record_media_input(0, *record, &source, "producer", "tex", timestamp);
                    if (publish) {
                        record->on_submitted([publish](const gpu::completion_s&) { publish(); });
                    }
                }
                (void)record->submit();
            }
            std::this_thread::sleep_for(16667us);
        }
    };
    feed();
    size_t reserved{};
    size_t active{};
    size_t blocked = sessions.size();
    size_t donor   = sessions.size();
    for (size_t index = 0; index < sessions.size(); ++index) {
        const auto metrics = sessions.at(index)->metrics().inputs;
        reserved += metrics.reserved_bytes;
        if (metrics.delivered != 0) {
            ++active;
            donor = index;
        } else {
            blocked = index;
            if (metrics.error.find("Shared input texture budget exhausted") == std::string::npos) {
                throw std::runtime_error("Shared-budget probe did not reach shared admission exhaustion");
            }
        }
    }
    if (active != 3 || reserved != 3ULL * 4096 * 4096 * 4 * 10) {
        throw std::runtime_error("Shared admission retained tentative charges or exceeded its bound");
    }
    const std::string stop = "() => { document.querySelector('video').srcObject.getTracks().forEach(t => t.stop()); }";
    run_script(sessions.at(donor), stop);
    wait_demand(sessions.at(donor), 0, true);
    feed();
    if (sessions.at(blocked)->metrics().inputs.delivered == 0 ||
        !sessions.at(blocked)->metrics().inputs.error.empty()) {
        throw std::runtime_error("Waiting browser session did not recover released shared admission");
    }
    for (const auto& session : sessions) {
        run_script(session, stop);
        wait_demand(session, 0, true);
    }
    std::cout << "Shared admission recovered between independent browser sessions\n";
}

void run(subsystem_s&       subsystem,
         gpu::device_s&     gpu,
         bool               small_inputs,
         const std::string& navigation_url,
         const std::string& profile,
         bool               budget)
{
    std::string page =
        R"HTML(
<!doctype html>
<style>
  html,
  body {
    margin: 0;
    width: 100%;
    height: 100%;
    overflow: hidden;
    background: transparent;
  }
  body {
    display: flex;
  }
  video {
    width: 80px;
    min-width: 0;
    height: 100%;
    object-fit: fill;
  }
</style>
)HTML";
    for (int input = 0; input < 8; ++input) {
        page += "<video muted autoplay playsinline></video>";
    }

    page +=
        R"HTML(
<script>
  document.querySelectorAll("video").forEach((v, inputIndex) =>
    miximus
      .getInputMediaStream({ inputIndex })
      .then((s) => {
        v.srcObject = s;
        return v.play();
      })
      .catch(console.error),
  );
</script>
)HTML";
    if (!navigation_url.empty()) {
        std::filesystem::create_directories(profile);
        std::ofstream output(std::filesystem::path(profile) / "inputs.html");
        output << page;
        if (!output) {
            throw std::runtime_error("Could not write navigation fixture");
        }
    }
    const session_s::options_s options{
        .url        = navigation_url.empty() ? "data:text/html," + CefURIEncode(page, false).ToString()
                                             : navigation_url + "/inputs.html",
        .dimensions = {640, 360},
        .frame_rate = 60,
    };
    auto request = subsystem.create_session(options);

    std::shared_ptr<session_s> session;
    const auto                 deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        session = request->session();
        if (session && session->media_input_demand() == 255) {
            break;
        }

        if (!request->error().empty()) {
            throw std::runtime_error(request->error());
        }

        std::this_thread::sleep_for(10ms);
    }

    if (!session || session->media_input_demand() != 255) {
        throw std::runtime_error("Session did not subscribe to eight inputs");
    }

    if (budget) {
        check_budget_recovery(gpu, session);
        check_shared_budget_recovery(subsystem, gpu);
        return;
    }
    frame_probe_s probe(gpu, session, small_inputs);
    if (!navigation_url.empty()) {
        for (int cycle = 0; cycle < 3; ++cycle) {
            probe.run_stage(0);
            auto       other_origin = navigation_url;
            const auto host         = other_origin.find("127.0.0.1");
            if (host == std::string::npos) {
                throw std::runtime_error("Navigation origin must use 127.0.0.1");
            }
            other_origin.replace(host, 9, "localhost");
            run_script(session, "() => { setTimeout(() => location.href = '" + other_origin + "/blank.html', 50); }");
            wait_demand(session, 0, true);
            const auto navigation_deadline = std::chrono::steady_clock::now() + 10s;
            while (!session->context_ready() && std::chrono::steady_clock::now() < navigation_deadline) {
                std::this_thread::sleep_for(10ms);
            }
            run_script(session, "() => { setTimeout(() => history.back(), 50); }");
            wait_demand(session, 255, false);
            probe.run_stage(0);
        }
        run_script(session,
                   "() => { document.querySelectorAll('video').forEach(v => v.srcObject.getTracks().forEach(t => "
                   "t.stop())); }");
        wait_demand(session, 0, true);
        return;
    }
    for (int stage = -1; stage < 3; ++stage) {
        if (stage == 0) {
            run_script(session, R"JS(
() => {
  globalThis.timestampSamples = [];
  const track = document.querySelectorAll("video")[1].srcObject.getVideoTracks()[0];
  globalThis.timestampReader = new MediaStreamTrackProcessor({ track }).readable.getReader();
  globalThis.timestampRead = (async () => {
    while (true) {
      const { value: frame, done } = await timestampReader.read();
      if (done) break;
      timestampSamples.push({ timestamp: frame.timestamp, width: frame.displayWidth });
      frame.close();
    }
  })();
}
)JS");
        }
        if (stage == 2 && !request->reload()) {
            throw std::runtime_error("Session reload was rejected");
        }

        probe.run_stage(stage);
        if (stage == 1) {
            run_script(session, R"JS(
async () => {
  await timestampReader.cancel();
  await timestampRead;
  let previous = -1;
  let connected = false;
  let disconnected = false;
  for (const sample of timestampSamples) {
    if (sample.timestamp <= previous) throw new Error("Media input timestamp did not advance");
    previous = sample.timestamp;
    if (sample.width !== 16) connected = true;
    else if (connected) disconnected = true;
  }
  if (!disconnected) throw new Error("Timestamp probe missed the disconnect transition");
  timestampReader.releaseLock();
}
)JS");
        }
    }

    run_script(session, R"JS(
async () => {
  const video = document.querySelector("video");
  const original = video.srcObject;
  const track = original.getVideoTracks()[0];
  original.removeTrack(track);
  const replacement = await miximus.getInputMediaStream({ inputIndex: 0 });
  if (replacement === original || replacement.getVideoTracks()[0] !== track) {
    throw new Error("Removing a track did not reconstruct the input stream");
  }
  const foreign = document.querySelectorAll("video")[1].srcObject.getVideoTracks()[0];
  replacement.removeTrack(track);
  replacement.addTrack(foreign);
  const restored = await miximus.getInputMediaStream({ inputIndex: 0 });
  if (restored.getVideoTracks()[0] !== track || restored.getTracks().length !== 1) {
    throw new Error("A foreign track was returned as input 0");
  }
  video.srcObject = restored;
  video.play().catch(console.error);
}
)JS");
    run_script(session, R"JS(
async () => {
  const video = document.querySelector("video");
  for (let cycle = 0; cycle < 32; ++cycle) {
    video.srcObject.getTracks().forEach(t => t.stop());
    video.srcObject = await miximus.getInputMediaStream({ inputIndex: 0 });
  }
  video.play().catch(console.error);
}
)JS");
    wait_demand(session, 255, false);
    probe.run_stage(0);

    run_script(session, R"JS(
() => {
  const videos = [...document.querySelectorAll("video")];
  globalThis.survivor = videos[0].srcObject.clone();
  for (const v of videos) {
    v.srcObject.getTracks().forEach((t) => t.stop());
    v.srcObject = null;
  }
}
)JS");
    wait_demand(session, 1, false);
    run_script(session, "() => { survivor.getTracks().forEach(t => t.stop()); globalThis.survivor=null; }");
    wait_demand(session, 0, true);
    const auto stopped_submissions = session->metrics().inputs.submitted;
    run_script(session, R"JS(
async () => {
  const stream = await miximus.getInputMediaStream({ inputIndex: 0 });
  const video = document.querySelector("video");
  video.srcObject = stream;
  video.play().catch(console.error);
}
)JS");
    wait_demand(session, 1, true);
    if (session->metrics().inputs.submitted != stopped_submissions) {
        throw std::runtime_error("Reacquired disconnected stream exported dummy frames");
    }

    run_script(session, "() => { document.querySelector('video').srcObject.getTracks().forEach(t => t.stop()); }");
    wait_demand(session, 0, true);
    // Reconnect real GPU inputs, then navigate to a document with no media API use.
    // This must retire the old document's destinations and its admission charge.
    if (!request->reload()) {
        throw std::runtime_error("Navigation regression reload was rejected");
    }
    wait_demand(session, 255, false);
    probe.run_stage(0);
    run_script(session, "() => { setTimeout(() => location.href = 'about:blank', 50); }");
    wait_demand(session, 0, true);
    session.reset();
    request.reset();
}
} // namespace

int main(int argc, char** argv)
{
    const bool small_inputs = argc == 4 && std::string_view(argv[3]) == "--small-inputs";
    const bool budget       = argc == 4 && std::string_view(argv[3]) == "--budget";
    const bool navigation   = argc == 5 && std::string_view(argv[3]) == "--navigation";
    if (argc != 3 && !small_inputs && !navigation && !budget) {
        std::cerr
            << "Usage: cef_media_input_session_probe RUNTIME PROFILE [--small-inputs | --budget | --navigation URL]\n";
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
            run(subsystem, gpu, small_inputs, navigation ? argv[4] : "", argv[2], budget);
        }

        if (gpu.validation_errors() != 0U) {
            throw std::runtime_error("Vulkan validation errors");
        }

        std::cout << "Requested session input regressions passed, including clean shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
