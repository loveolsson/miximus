#include "runtime.hpp"

#include "include/cef_app.h"
#include "include/cef_request_context.h"
#include "task.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <stdexcept>
#include <thread>

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

class cache_clear_callback_s final : public CefCompletionCallback
{
    std::shared_ptr<std::atomic_bool> pending_;
    IMPLEMENT_REFCOUNTING(cache_clear_callback_s);

  public:
    explicit cache_clear_callback_s(std::shared_ptr<std::atomic_bool> pending)
        : pending_(std::move(pending))
    {
    }
    void OnComplete() override { *pending_ = false; }
};

class browser_app_s final
    : public CefApp
    , public CefBrowserProcessHandler
{
    IMPLEMENT_REFCOUNTING(browser_app_s);

    std::mutex              mutex;
    std::condition_variable ready;
    bool                    initialized{};

  public:
    bool wait_initialized()
    {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, std::chrono::seconds(10), [&] { return initialized; });
    }

    void OnBeforeCommandLineProcessing(const CefString& /* process_type */,
                                       CefRefPtr<CefCommandLine> command_line) override
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
            std::scoped_lock lock(mutex);
            initialized = true;
        }
        ready.notify_all();
    }
};
} // namespace

struct runtime_s::state_s
{
    std::shared_ptr<std::atomic_bool> cache_clear_pending = std::make_shared<std::atomic_bool>(false);
    CefRefPtr<browser_app_s>          app                 = new browser_app_s;
    std::thread::id                   owner               = std::this_thread::get_id();
};

runtime_s::runtime_s(const std::filesystem::path& runtime_directory, const std::filesystem::path& profile_directory)
    : state_(std::make_unique<state_s>())
{
    const auto runtime = std::filesystem::canonical(runtime_directory);
    const auto profile = std::filesystem::absolute(profile_directory);
    std::filesystem::create_directories(profile);
    CefSettings settings;
    settings.multi_threaded_message_loop  = 1;
    settings.windowless_rendering_enabled = 1;
    settings.command_line_args_disabled   = 1;
    // Keep the host application's existing SIGINT/SIGTERM handlers in charge.
    settings.disable_signal_handlers             = 1;
    CefString(&settings.browser_subprocess_path) = (runtime / "miximus_cef_helper").string();
    CefString(&settings.resources_dir_path)      = runtime.string();
    CefString(&settings.locales_dir_path)        = (runtime / "locales").string();
    CefString(&settings.root_cache_path)         = profile.string();
    CefString(&settings.log_file)                = (profile / "cef.log").string();
    std::array<char, 8>       name{"miximus"};
    std::array<char*, 2>      argv{name.data(), nullptr};
    const CefMainArgs         args(1, argv.data());
    const preserved_signals_s application_signals;
    if (!CefInitialize(args, settings, state_->app, nullptr)) {
        throw std::runtime_error("CEF initialization failed");
    }
    if (!state_->app->wait_initialized()) {
        CefShutdown();
        throw std::runtime_error("CEF context initialization timed out");
    }
}

bool runtime_s::clear_http_cache()
{
    const auto pending = state_->cache_clear_pending;
    if (pending->exchange(true)) {
        return false;
    }
    if (!CefPostTask(TID_UI, new task_s([pending] {
                         CefRequestContext::GetGlobalContext()->ClearHttpCache(new cache_clear_callback_s(pending));
                     }))) {
        *pending = false;
        throw std::runtime_error("Cannot dispatch CEF cache clearing");
    }
    return true;
}

bool runtime_s::cache_clear_pending() const { return *state_->cache_clear_pending; }

runtime_s::~runtime_s()
{
    if (std::this_thread::get_id() != state_->owner) {
        std::terminate();
    }
    CefShutdown();
}

} // namespace miximus::nodes::cef::detail
