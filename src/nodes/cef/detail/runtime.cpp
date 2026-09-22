#include "runtime.hpp"

#include "include/cef_app.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace miximus::nodes::cef::detail {
namespace {
class browser_app_s final
    : public CefApp
    , public CefBrowserProcessHandler
{
    IMPLEMENT_REFCOUNTING(browser_app_s);

  public:
    std::mutex              mutex;
    std::condition_variable ready;
    bool                    initialized{};

    void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command_line) override
    {
        // Match the application's existing X11/XWayland platform. This switch
        // affects Chromium only; no GLFW or process-wide environment changes.
        command_line->AppendSwitchWithValue("ozone-platform", "x11");
        // Linux shared-texture OSR needs ANGLE's native EGL import path.
        command_line->AppendSwitchWithValue("use-angle", "gl-egl");
        // Embedded sources must never show standalone Chrome onboarding or
        // default-browser prompts, including for a fresh per-instance profile.
        command_line->AppendSwitch("no-first-run");
        command_line->AppendSwitch("no-default-browser-check");
    }

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void                                OnContextInitialized() override
    {
        {
            std::lock_guard lock(mutex);
            initialized = true;
        }
        ready.notify_all();
    }
};
} // namespace

struct runtime_s::state_s
{
    CefRefPtr<browser_app_s> app   = new browser_app_s;
    std::thread::id          owner = std::this_thread::get_id();
};

runtime_s::runtime_s(const std::filesystem::path& runtime_directory, const std::filesystem::path& profile_directory)
    : state_(std::make_unique<state_s>())
{
    const auto runtime = std::filesystem::canonical(runtime_directory);
    const auto profile = std::filesystem::absolute(profile_directory);
    std::filesystem::create_directories(profile);
    CefSettings settings;
    settings.multi_threaded_message_loop         = true;
    settings.windowless_rendering_enabled        = true;
    settings.command_line_args_disabled          = true;
    CefString(&settings.browser_subprocess_path) = (runtime / "miximus_cef_helper").string();
    CefString(&settings.resources_dir_path)      = runtime.string();
    CefString(&settings.locales_dir_path)        = (runtime / "locales").string();
    CefString(&settings.root_cache_path)         = profile.string();
    CefString(&settings.log_file)                = (profile / "cef.log").string();
    char              name[]                     = "miximus";
    char*             argv[]                     = {name, nullptr};
    const CefMainArgs args(1, argv);
    if (!CefInitialize(args, settings, state_->app, nullptr)) {
        throw std::runtime_error("CEF initialization failed");
    }
    std::unique_lock lock(state_->app->mutex);
    if (!state_->app->ready.wait_for(lock, std::chrono::seconds(10), [&] { return state_->app->initialized; })) {
        lock.unlock();
        CefShutdown();
        throw std::runtime_error("CEF context initialization timed out");
    }
}

runtime_s::~runtime_s()
{
    if (std::this_thread::get_id() != state_->owner) {
        std::terminate();
    }
    CefShutdown();
}

} // namespace miximus::nodes::cef::detail
