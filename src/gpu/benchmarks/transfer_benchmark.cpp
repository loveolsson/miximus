#include "gpu/device.hpp"
#include "gpu/texture_frame.hpp"
#include "gpu/transfer/detail/frame_staging.hpp"
#include "logger/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace miximus;
using namespace gpu;
using namespace gpu::transfer;
namespace {

using benchmark_clock_t = std::chrono::steady_clock;
using staging_t         = gpu::transfer::detail::frame_staging_s;

template <class Predicate>
void await(Predicate predicate)
{
    const auto deadline = benchmark_clock_t::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (benchmark_clock_t::now() >= deadline) {
            throw std::runtime_error("transfer benchmark completion timed out");
        }

        std::this_thread::yield();
    }
}

nlohmann::json measure(staging_t& staging, staging_t::direction_e direction, uint32_t iterations)
{
    // Allocation, host population and first-use initialization are outside measurement.
    std::vector<double> samples;
    samples.reserve(iterations);
    for (uint32_t i = 0; i < iterations + 30; ++i) {
        const auto start = benchmark_clock_t::now();
        if (direction == staging_t::direction_e::cpu_to_gpu) {
            staging.prepare_host_write();
        }

        await([&] { return staging.submit_transfer(); });
        await([&] { return staging.transfer_ready(); });
        const double elapsed = std::chrono::duration<double, std::micro>(benchmark_clock_t::now() - start).count();
        if (i >= 30) {
            samples.push_back(elapsed);
        }
    }

    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::ranges::sort(samples);
    auto percentile = [&](double p) {
        return samples[static_cast<size_t>(std::ceil(p * static_cast<double>(samples.size()))) - 1];
    };
    return {
        {"direction",               direction == staging_t::direction_e::cpu_to_gpu ? "upload" : "readback"},
        {"backend",                 staging.backend_name()                                                 },
        {"samples",                 samples.size()                                                         },
        {"mean_us",                 total / static_cast<double>(samples.size())                            },
        {"p50_us",                  percentile(0.50)                                                       },
        {"p95_us",                  percentile(0.95)                                                       },
        {"p99_us",                  percentile(0.99)                                                       },
        {"effective_GB_per_second",
         double(staging.host_buffer_size_bytes()) * static_cast<double>(samples.size()) / (total * 1000)   }
    };
}
struct probe_options_s
{
    device_options_s options{};
    uint32_t         iterations{500};
    std::string      output;
};

probe_options_s parse_options(int argc, char** argv)
{
    probe_options_s result;
    auto& [options, iterations, output] = result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg   = argv[i];
        auto                   value = [&]() -> std::string {
            if (++i == argc) {
                throw std::invalid_argument("missing option value");
            }

            return argv[i];
        };

        if (arg == "--use-cuda") {
            options.use_cuda = true;
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--device") {
            options.device_uuid = value();
        } else if (arg == "--output") {
            output = value();
        } else if (arg == "--iterations") {
            const auto text = value();
            size_t     end{};
            const auto count = std::stoul(text, &end);
            if (end != text.size() || count == 0 || count > 100000) {
                throw std::invalid_argument("iterations must be between 1 and 100000");
            }

            iterations = static_cast<uint32_t>(count);
        } else {
            throw std::invalid_argument("usage: miximus_transfer_benchmark [--use-cuda] [--validation] "
                                        "[--device UUID] [--iterations N] [--output FILE]");
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    logger::init_loggers(spdlog::level::warn);
    try {
        const auto [options, iterations, output] = parse_options(argc, argv);

        device_s device(options);
        auto     report   = nlohmann::json::parse(device.diagnostics_json());
        report["method"]  = "Sequential production frame_staging transfers; host-observed submit-to-ready latency, "
                            "including ownership hand-offs and polling; 30 warm-up transfers per case; "
                            "host filling, allocation, color conversion, SDK and service queue delay excluded";
        report["results"] = nlohmann::json::array();
        for (const vec2i_t size : {
                 vec2i_t{1280, 720 },
                 vec2i_t{1920, 1080},
                 vec2i_t{3840, 2160}
        }) {
            for (const auto format : {host_pixel_format_e::rgba_u8, host_pixel_format_e::v210}) {
                const size_t stride =
                    format == host_pixel_format_e::v210 ? (size_t(size.x) + 47) / 48 * 128 : size_t(size.x) * 4;
                host_frame_layout_s layout{.image_dimensions        = size,
                                           .pixel_format            = format,
                                           .row_stride_bytes        = stride,
                                           .buffer_size_bytes       = stride * size.y,
                                           .address_alignment_bytes = 4096};
                auto                plan = gpu::transfer::detail::make_texture_transfer_plan(layout);
                texture_frame_s     frame(device, layout, sampling_e::linear);
                staging_t           upload(device, plan, staging_t::direction_e::cpu_to_gpu, &frame);
                plan.host_layout.memory_access = host_memory_access_e::read_only;
                staging_t readback(device, plan, staging_t::direction_e::gpu_to_cpu, &frame);
                // Distinct deterministic bytes detect failed copies and row/pitch corruption.
                std::vector<std::byte> reference(layout.buffer_size_bytes);
                for (size_t i = 0; i < reference.size(); ++i) {
                    reference[i] = std::byte((i * 17 + i / stride * 13) % 251);
                }

                upload.prepare_host_write();
                std::memcpy(upload.host_memory(), reference.data(), reference.size());
                auto upload_result   = measure(upload, staging_t::direction_e::cpu_to_gpu, iterations);
                auto readback_result = measure(readback, staging_t::direction_e::gpu_to_cpu, iterations);
                if (std::memcmp(readback.host_memory(), reference.data(), reference.size()) != 0) {
                    throw std::runtime_error("benchmark readback did not match uploaded bytes");
                }

                for (auto* item : {&upload_result, &readback_result}) {
                    (*item)["width"]           = size.x;
                    (*item)["height"]          = size.y;
                    (*item)["format"]          = format == host_pixel_format_e::v210 ? "v210" : "RGBA8";
                    (*item)["bytes"]           = reference.size();
                    (*item)["row_stride"]      = stride;
                    (*item)["pixels_verified"] = true;
                    report["results"].push_back(*item);
                }

                device.collect();
            }
        }

        report["validation_errors"] = device.validation_errors();
        if (device.validation_errors() != 0U) {
            throw std::runtime_error("Vulkan validation errors during benchmark");
        }

        if (!output.empty()) {
            std::ofstream file(output);
            file << report.dump(2) << '\n';
            if (!file) {
                throw std::runtime_error("could not write benchmark report");
            }
        }

        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
