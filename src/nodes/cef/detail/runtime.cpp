#include "runtime.hpp"

#include "include/cef_app.h"
#include "include/cef_request_context.h"
#include "runtime_platform.hpp"
#include "task.hpp"
#include "utils/failure_shutdown.hpp"
#include "wrapper/cef/platform.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace miximus::nodes::cef::detail {
namespace {
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
        configure_runtime_command_line(command_line);
        // Embedded sources must never show standalone Chrome onboarding or
        // default-browser prompts, including for a fresh per-instance profile.
        command_line->AppendSwitch("no-first-run");
        command_line->AppendSwitch("no-default-browser-check");
        // Embedded sources do not need Chrome's background USB landing-page
        // discovery. Explicit page-initiated WebUSB access remains separate.
        auto disabled_features = command_line->GetSwitchValue("disable-features").ToString();
        if (!disabled_features.empty()) {
            disabled_features += ',';
        }
        disabled_features += "WebUsbDeviceDetection";
        command_line->AppendSwitchWithValue("disable-features", disabled_features);
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

runtime_s::runtime_s(const std::filesystem::path& runtime_directory,
                     const std::filesystem::path& profile_directory,
                     bool                         disable_sandbox)
    : state_(std::make_unique<state_s>())
{
    const auto runtime = std::filesystem::canonical(runtime_directory);
    const auto profile = std::filesystem::absolute(profile_directory);
    std::filesystem::create_directories(profile);
    cef_wrapper::load_runtime(runtime);
    CefSettings settings;
    settings.no_sandbox                   = static_cast<int>(disable_sandbox);
    settings.multi_threaded_message_loop  = 1;
    settings.windowless_rendering_enabled = 1;
    settings.command_line_args_disabled   = 1;
    // Keep the host application's existing SIGINT/SIGTERM handlers in charge.
    settings.disable_signal_handlers             = 1;
    const auto helper                            = runtime / MIXIMUS_CEF_HELPER_NAME;
    CefString(&settings.browser_subprocess_path) = helper.native();
    CefString(&settings.resources_dir_path)      = runtime.native();
    CefString(&settings.locales_dir_path)        = (runtime / "locales").native();
    CefString(&settings.root_cache_path)         = profile.native();
    CefString(&settings.log_file)                = (profile / "cef.log").native();
    const runtime_initialization_s initialization;
    if (!initialize_runtime(settings, state_->app, runtime)) {
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
        utils::fail_without_unwinding("CEF runtime destroyed on the wrong thread");
    }
    CefShutdown();
}

} // namespace miximus::nodes::cef::detail
