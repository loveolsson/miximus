#include "gpu/device.hpp"
#include "logger/logger.hpp"
#include "nodes/cef/detail/runtime.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: cef_runtime_probe RUNTIME_DIRECTORY PROFILE_DIRECTORY\n";
        return 2;
    }
    std::cout.setf(std::ios::unitbuf);
    try {
        miximus::logger::init_loggers(spdlog::level::warn);
        miximus::gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        miximus::gpu::device_s gpu(options);
        auto                   loader_path = [] {
            auto* library = dlopen("libvulkan.so.1", RTLD_NOLOAD | RTLD_NOW);
            if (!library) {
                throw std::runtime_error("Vulkan loader is not loaded");
            }
            Dl_info           info{};
            const bool        found = dladdr(dlsym(library, "vkGetInstanceProcAddr"), &info) != 0;
            const std::string path  = found ? info.dli_fname : "";
            dlclose(library);
            return std::filesystem::canonical(path);
        };
        const auto before = loader_path();
        std::cout << "Miximus Vulkan loader: " << before << '\n';
        {
            miximus::nodes::cef::detail::runtime_s runtime(argv[1], argv[2]);
            std::cout << "CEF threaded runtime initialized\n";
            if (loader_path() != before) {
                throw std::runtime_error("CEF changed the Vulkan loader");
            }
        }
        if (gpu.validation_errors() != 0) {
            throw std::runtime_error("Vulkan validation failed");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "CEF runtime shut down\n";
}
