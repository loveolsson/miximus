#include "include/cef_version_info.h"
#include "platform.hpp"

#include <dlfcn.h>
#include <stdexcept>

namespace miximus::cef_wrapper {

std::filesystem::path runtime_directory()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&cef_version_info), &info) == 0 || info.dli_fname == nullptr) {
        throw std::runtime_error("Cannot locate the loaded CEF runtime");
    }

    return std::filesystem::canonical(info.dli_fname).parent_path();
}

std::filesystem::path vulkan_loader_path()
{
    auto* library = dlopen("libvulkan.so.1", RTLD_NOLOAD | RTLD_NOW);
    if (!library) {
        throw std::runtime_error("Vulkan loader is not loaded");
    }

    Dl_info           info{};
    const bool        found = dladdr(dlsym(library, "vkGetInstanceProcAddr"), &info) != 0;
    const std::string path  = found ? info.dli_fname : "";
    dlclose(library);
    return std::filesystem::canonical(path);
}

install_media_inputs_t find_install_media_inputs()
{
    return reinterpret_cast<install_media_inputs_t>(dlsym(RTLD_DEFAULT, INSTALL_MEDIA_INPUTS));
}

send_media_frame_t find_send_media_frame()
{
    return reinterpret_cast<send_media_frame_t>(dlsym(RTLD_DEFAULT, SEND_MEDIA_FRAME));
}

void load_runtime(const std::filesystem::path& /* directory */)
{
    // libcef is linked directly on Linux for its close interposer.
}

int execute_subprocess(int argc, char** argv, const CefRefPtr<CefApp>& app)
{
    const CefMainArgs args(argc, argv);
    return CefExecuteProcess(args, app, nullptr);
}

} // namespace miximus::cef_wrapper
