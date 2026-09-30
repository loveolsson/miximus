#include "runtime_platform.hpp"

#include <Windows.h>

namespace miximus::nodes::cef::detail {

struct runtime_initialization_s::state_s
{
};

runtime_initialization_s::runtime_initialization_s()  = default;
runtime_initialization_s::~runtime_initialization_s() = default;

void configure_runtime_command_line(const CefRefPtr<CefCommandLine>& command_line)
{
    command_line->AppendSwitchWithValue("use-angle", "d3d11");
}

bool initialize_runtime(CefSettings& settings, const CefRefPtr<CefApp>& app, const std::filesystem::path& /* runtime */)
{
    const CefMainArgs args(GetModuleHandleW(nullptr));
    return CefInitialize(args, settings, app, nullptr);
}

} // namespace miximus::nodes::cef::detail
