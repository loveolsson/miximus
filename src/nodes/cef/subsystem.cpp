#include "subsystem.hpp"

#include "detail/browser_session.hpp"
#include "detail/runtime.hpp"
#include "include/cef_version_info.h"
#include "utils/serial_executor.hpp"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace miximus::nodes::cef {
namespace {
using namespace std::chrono_literals;
using session_t                 = detail::browser_session_s;
constexpr size_t MAX_SESSIONS   = 16;
constexpr size_t TEXTURE_BUDGET = 2ULL * 1024 * 1024 * 1024;

std::filesystem::path runtime_directory()
{
    Dl_info info{};
    if ((dladdr(reinterpret_cast<const void*>(&cef_version_info), &info) == 0) || (info.dli_fname == nullptr)) {
        throw std::runtime_error("Cannot locate the loaded CEF runtime");
    }
    return std::filesystem::canonical(info.dli_fname).parent_path();
}

std::filesystem::path qualified_runtime(gpu::device_s& device, const std::filesystem::path& directory)
{
    if (!MIXIMUS_CEF_NATIVE_CAPTURE_READY) {
        throw std::runtime_error("CEF requires the verified native-handle completion SDK");
    }
    if (!device.external_image_import_support().enabled) {
        throw std::runtime_error("The selected GPU does not support accelerated CEF image import");
    }
    return directory;
}
} // namespace

struct session_request_s::state_s : std::enable_shared_from_this<state_s>
{
    mutable std::mutex                      mutex;
    std::weak_ptr<utils::serial_executor_s> executor;
    std::shared_ptr<session_s>              published;
    // Only the serial control worker touches owned.
    std::shared_ptr<session_t> owned;
    std::string                failure;
    size_t                     bytes{};
    bool                       cancelled{};

    void stop()
    {
        {
            const std::scoped_lock lock(mutex);
            if (cancelled) {
                return;
            }
            cancelled = true;
            published.reset();
        }
        if (auto control = executor.lock()) {
            control->post([self = shared_from_this()] {
                if (!self->owned) {
                    return;
                }
                self->owned->close_async();
                while (!self->owned->wait_closed(100ms)) {
                }
                // Publication is withdrawn before retirement. No new consumers
                // can acquire the session; existing render-thread uses finish
                // normally, without transferring queue ownership concurrently.
                while (self->owned.use_count() != 1) {
                    std::this_thread::sleep_for(1ms);
                }
                self->owned->reset_frames();
                while (!self->owned->resources_idle()) {
                    std::this_thread::sleep_for(1ms);
                }
                self->owned.reset();
            });
        }
    }
};

struct subsystem_s::impl_s
{
    gpu::device_s&                            device;
    detail::runtime_s                         runtime;
    std::shared_ptr<utils::serial_executor_s> executor = std::make_shared<utils::serial_executor_s>();
    // Admission is render-thread owned. Weak entries expire only after both
    // the node request and its queued allocation/retirement work have ended.
    std::vector<std::weak_ptr<session_request_s::state_s>> requests;

    impl_s(gpu::device_s& gpu, const std::filesystem::path& profile, const std::filesystem::path& directory)
        : device(gpu)
        , runtime(qualified_runtime(gpu, directory), profile)
    {
    }

    impl_s(const impl_s& other)            = delete;
    impl_s& operator=(const impl_s& other) = delete;
    impl_s(impl_s&& other)                 = delete;
    impl_s& operator=(impl_s&& other)      = delete;

    ~impl_s()
    {
        for (auto& request : requests) {
            if (auto state = request.lock()) {
                state->stop();
            }
        }
        // Drain every accepted control task before same-thread CefShutdown.
        executor.reset();
    }
};

session_request_s::session_request_s(std::shared_ptr<state_s> state)
    : state_(std::move(state))
{
}
session_request_s::~session_request_s() { state_->stop(); }

std::shared_ptr<session_s> session_request_s::session() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->published;
}

std::string session_request_s::error() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->failure;
}

subsystem_s::subsystem_s(gpu::device_s& device, const std::filesystem::path& profile)
    : subsystem_s(device, profile, runtime_directory())
{
}

subsystem_s::subsystem_s(gpu::device_s&               device,
                         const std::filesystem::path& profile,
                         const std::filesystem::path& directory)
    : impl_(std::make_unique<impl_s>(device, profile, directory))
{
}

subsystem_s::~subsystem_s() = default;

std::unique_ptr<session_request_s> subsystem_s::create_session(session_t::options_s options)
{
    auto state  = std::make_shared<session_request_s::state_s>();
    auto result = std::unique_ptr<session_request_s>(new session_request_s(state));
    std::erase_if(impl_->requests, [](const auto& request) { return request.expired(); });
    size_t reserved{};
    for (const auto& request : impl_->requests) {
        if (const auto entry = request.lock()) {
            reserved += entry->bytes;
        }
    }
    try {
        state->bytes = session_t::texture_budget(options);
        if (impl_->requests.size() >= MAX_SESSIONS || state->bytes > TEXTURE_BUDGET - reserved) {
            throw std::runtime_error("CEF session capacity or texture budget exhausted");
        }
    } catch (const std::exception& failure) {
        state->failure = failure.what();
        return result;
    }
    state->executor = impl_->executor;
    impl_->requests.push_back(state);
    impl_->executor->post([state, device = &impl_->device, options = std::move(options)]() mutable {
        {
            const std::scoped_lock lock(state->mutex);
            if (state->cancelled) {
                return;
            }
        }
        try {
            state->owned = std::make_shared<session_t>(*device, std::move(options));
            state->owned->start_async();
            const std::scoped_lock lock(state->mutex);
            if (!state->cancelled) {
                state->published = state->owned;
            }
        } catch (const std::exception& failure) {
            const std::scoped_lock lock(state->mutex);
            state->failure = failure.what();
        }
    });
    return result;
}

} // namespace miximus::nodes::cef
