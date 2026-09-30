#pragma once

#include "include/cef_app.h"

#include <filesystem>
#include <memory>

namespace miximus::nodes::cef::detail {

// Preserve platform process state until CEF's context has finished initializing.
class runtime_initialization_s
{
    struct state_s;
    std::unique_ptr<state_s> state_;

  public:
    runtime_initialization_s();
    ~runtime_initialization_s();
};

void configure_runtime_command_line(const CefRefPtr<CefCommandLine>& command_line);
bool initialize_runtime(CefSettings& settings, const CefRefPtr<CefApp>& app, const std::filesystem::path& runtime);

} // namespace miximus::nodes::cef::detail
