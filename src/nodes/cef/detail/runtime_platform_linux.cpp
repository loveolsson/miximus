#include "runtime_platform.hpp"

#include <array>
#include <csignal>
#include <exception>
#include <stdexcept>

namespace miximus::nodes::cef::detail {
namespace {

// Chrome's browser main parts install shutdown handlers independently of
// CefSettings::disable_signal_handlers. Preserve the embedding application's
// dispositions after initialization, without changing its signal/main loop.
class preserved_signals_s
{
    static constexpr std::array                  signals{SIGINT, SIGTERM, SIGHUP};
    std::array<struct sigaction, signals.size()> actions_{};

  public:
    preserved_signals_s()
    {
        for (size_t index = 0; index < signals.size(); ++index) {
            if (sigaction(signals.at(index), nullptr, &actions_.at(index)) != 0) {
                throw std::runtime_error("Cannot preserve application signal handlers before CEF startup");
            }
        }
    }
    preserved_signals_s(const preserved_signals_s& other)            = delete;
    preserved_signals_s& operator=(const preserved_signals_s& other) = delete;
    preserved_signals_s(preserved_signals_s&& other)                 = delete;
    preserved_signals_s& operator=(preserved_signals_s&& other)      = delete;

    ~preserved_signals_s()
    {
        for (size_t index = 0; index < signals.size(); ++index) {
            if (sigaction(signals.at(index), &actions_.at(index), nullptr) != 0) {
                std::terminate();
            }
        }
    }
};

} // namespace

struct runtime_initialization_s::state_s
{
    preserved_signals_s signals;
};

runtime_initialization_s::runtime_initialization_s()
    : state_(std::make_unique<state_s>())
{
}

runtime_initialization_s::~runtime_initialization_s() = default;

void configure_runtime_command_line(const CefRefPtr<CefCommandLine>& command_line)
{
    // Match the application's existing X11/XWayland platform. This switch
    // affects Chromium only; no GLFW or process-wide environment changes.
    command_line->AppendSwitchWithValue("ozone-platform", "x11");
    // Linux shared-texture OSR needs ANGLE's native EGL import path.
    command_line->AppendSwitchWithValue("use-angle", "gl-egl");
}

bool initialize_runtime(CefSettings& settings, const CefRefPtr<CefApp>& app, const std::filesystem::path& /* runtime */)
{
    std::array<char, 8>  name{"miximus"};
    std::array<char*, 2> argv{name.data(), nullptr};
    const CefMainArgs    args(1, argv.data());
    return CefInitialize(args, settings, app, nullptr);
}

} // namespace miximus::nodes::cef::detail
