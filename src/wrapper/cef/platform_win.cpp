#include "include/wrapper/cef_library_loader.h"
#include "platform.hpp"

#include <Windows.h>
#include <array>
#include <stdexcept>

namespace miximus::cef_wrapper {
namespace {

std::filesystem::path module_path(const wchar_t* name)
{
    const auto                 module = GetModuleHandleW(name);
    std::array<wchar_t, 32768> path{};
    const auto                 length =
        module != nullptr ? GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())) : 0;

    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Cannot locate loaded runtime library");
    }

    return std::filesystem::canonical(std::wstring(path.data(), length));
}

} // namespace

std::filesystem::path runtime_directory() { return module_path(nullptr).parent_path() / "cef"; }
std::filesystem::path vulkan_loader_path() { return module_path(L"vulkan-1.dll"); }

install_media_inputs_t find_install_media_inputs()
{
    const auto module = GetModuleHandleW(L"libcef.dll");
    return module != nullptr ? reinterpret_cast<install_media_inputs_t>(GetProcAddress(module, INSTALL_MEDIA_INPUTS))
                             : nullptr;
}

send_media_frame_t find_send_media_frame()
{
    const auto module = GetModuleHandleW(L"libcef.dll");
    return module != nullptr ? reinterpret_cast<send_media_frame_t>(GetProcAddress(module, SEND_MEDIA_FRAME)) : nullptr;
}

void load_runtime(const std::filesystem::path& directory)
{
    // Keep CEF loaded until process exit, including all wrapper destructors.
    static CefScopedLibraryLoader loader;
    static const bool             loaded = [&directory] {
        cef_version_info_t version{};
        CEF_POPULATE_VERSION_INFO(&version);
        return loader.LoadInMainAssert((directory / "libcef.dll").c_str(), nullptr, true, &version);
    }();
    (void)loaded;
}

int execute_subprocess(int /* argc */, char** /* argv */, const CefRefPtr<CefApp>& app)
{
    load_runtime(module_path(nullptr).parent_path());
    const CefMainArgs args(GetModuleHandleW(nullptr));
    return CefExecuteProcess(args, app, nullptr);
}

} // namespace miximus::cef_wrapper
