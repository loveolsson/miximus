#include "gpu/device.hpp"
#include "logger/logger.hpp"
#include "nodes/cef/detail/runtime.hpp"
#include "wrapper/cef/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    try {
        if (argc != 3) {
            std::cerr << "Usage: cef_runtime_probe RUNTIME_DIRECTORY PROFILE_DIRECTORY\n";
            return 2;
        }
        std::cout.setf(std::ios::unitbuf);
        miximus::logger::init_loggers(spdlog::level::warn);
        miximus::gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        miximus::gpu::device_s gpu(options);
        const auto             before = miximus::cef_wrapper::vulkan_loader_path();
        std::cout << "Miximus Vulkan loader: " << before << '\n';
        {
            miximus::nodes::cef::detail::runtime_s runtime(argv[1], argv[2]);
            std::cout << "CEF threaded runtime initialized\n";
            if (miximus::cef_wrapper::vulkan_loader_path() != before) {
                throw std::runtime_error("CEF changed the Vulkan loader");
            }
        }
        if (gpu.validation_errors() != 0) {
            throw std::runtime_error("Vulkan validation failed");
        }
        std::cout << "CEF runtime shut down\n";
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("CEF probe failed with an unknown exception\n", stderr);
        return 1;
    }
}
