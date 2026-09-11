#include "window.hpp"

#include "detail/monitor_platform.hpp"
#include "logger/logger.hpp"
#include "static_files/files.hpp"
#include "wrapper/stb/image.hpp"

#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace miximus;

const auto _log = [] { return getlog("gpu"); };

stb::decoded_image_s load_image(std::string_view filename)
{
    const auto file_data = static_files::get_resource_files().get_file_or_throw(filename).unzip();
    auto       image     = stb::decode_image(std::as_bytes(std::span{file_data}), stb::image_channels_e::rgba);

    if (image.source_channels() != 4) {
        throw std::runtime_error(std::format("Logo {} is not RGBA", filename));
    }

    return image;
}

} // namespace

namespace miximus::gpu {

void window_s::monitor_config_callback(GLFWmonitor* monitor, int event) noexcept
{
    try {
        const std::scoped_lock lock(monitor_mutex_);
        bool                   changed = false;
        if (event == GLFW_CONNECTED) {
            const auto* raw_label = glfwGetMonitorName(monitor);
            const auto  id        = detail::get_monitor_id(monitor);
            const auto  label     = raw_label != nullptr ? std::string(raw_label) : id;
            changed               = monitors_.emplace(id, monitor_record_s{.label = label, .handle = monitor}).second;
            _log()->info("Monitor connected: {} ({})", label, id);
        } else {
            const auto it = std::ranges::find_if(
                monitors_, [monitor](const auto& entry) { return entry.second.handle == monitor; });
            if (it != monitors_.end()) {
                _log()->info("Monitor disconnected: {} ({})", it->second.label, it->first);
                monitors_.erase(it);
                changed = true;
            }
        }

        if (changed) {
            monitor_list_version_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        logger::log_error_noexcept("gpu", "GLFW monitor callback failed");
    }
}

uint64_t window_s::get_monitor_list_version() noexcept { return monitor_list_version_.load(std::memory_order_relaxed); }

std::vector<settings_option_s> window_s::get_monitors()
{
    const std::scoped_lock         lock(monitor_mutex_);
    std::vector<settings_option_s> monitors;
    monitors.reserve(monitors_.size());
    for (const auto& [id, monitor] : monitors_) {
        monitors.push_back({.id = id, .label = monitor.label});
    }

    return monitors;
}

std::optional<int> window_s::get_monitor_refresh_rate(std::string_view monitor_id)
{
    const std::scoped_lock lock(monitor_mutex_);
    GLFWmonitor*           monitor{};
    if (const auto it = monitors_.find(monitor_id); it != monitors_.end()) {
        monitor = it->second.handle;
    } else if (monitor_id.empty()) {
        monitor = glfwGetPrimaryMonitor();
    }

    if (monitor == nullptr) {
        return std::nullopt;
    }

    const auto* mode = glfwGetVideoMode(monitor);
    if (mode == nullptr || mode->refreshRate <= 0) {
        return std::nullopt;
    }

    return mode->refreshRate;
}

void window_s::initialize_glfw()
{
    static std::once_flag glfw_init;
    std::call_once(glfw_init, []() {
        _log()->debug("Initializing GLFW");

#ifdef __linux__
        // Preserve screen-output pixel geometry and absolute desktop positioning.
        // XWayland provides this on Wayland desktops; rendering remains Vulkan.
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

        if (glfwInit() == GLFW_FALSE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwSetErrorCallback([](int code, const char* message) {
            logger::log_error_noexcept("gpu", "GLFW {}: {}", code, message ? message : "unknown error");
        });
        glfwSetMonitorCallback(window_s::monitor_config_callback);

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        // glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);

        int  count    = 0;
        auto monitors = glfwGetMonitors(&count);
        for (int i = 0; i < count; ++i) {
            auto*       monitor   = monitors[i];
            const auto* raw_label = glfwGetMonitorName(monitor);
            const auto  id        = detail::get_monitor_id(monitor);
            const auto  label     = raw_label != nullptr ? std::string(raw_label) : id;

            _log()->info("Found monitor: {} ({})", label, id);

            monitors_.emplace(id, monitor_record_s{.label = label, .handle = monitor});
        }

        monitor_list_version_.fetch_add(1, std::memory_order_relaxed);
    });
}

void window_s::configure_window_hints()
{
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    glfwWindowHint(GLFW_REFRESH_RATE, GLFW_DONT_CARE);
}

auto window_s::resolve_window_target(const window_settings_s& settings) -> window_target_s
{
    const std::scoped_lock lock(monitor_mutex_);
    window_target_s        target{.dimensions = settings.rect.size};
    if (settings.fullscreen) {
        auto it = monitors_.find(settings.monitor_id);
        if (it == monitors_.end()) {
            // Compatibility for settings saved before monitor IDs and labels were separated.
            it = std::ranges::find_if(
                monitors_, [&settings](const auto& entry) { return entry.second.label == settings.monitor_id; });
        }

        if (it != monitors_.end()) {
            target.monitor = it->second.handle;
        } else if (settings.monitor_id.empty()) {
            target.monitor = glfwGetPrimaryMonitor();
        }

        if (target.monitor != nullptr) {
            target.mode = glfwGetVideoMode(target.monitor);
            if (target.mode != nullptr) {
                glfwWindowHint(GLFW_RED_BITS, target.mode->redBits);
                glfwWindowHint(GLFW_GREEN_BITS, target.mode->greenBits);
                glfwWindowHint(GLFW_BLUE_BITS, target.mode->blueBits);
                glfwWindowHint(GLFW_REFRESH_RATE, target.mode->refreshRate);
                target.dimensions = {target.mode->width, target.mode->height};
            }
        }
    }

    return target;
}

void window_s::configure_visible_window(const window_settings_s& settings, const window_target_s& target)
{
    if (target.mode == nullptr && glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        glfwSetWindowPos(window_, settings.rect.pos.x, settings.rect.pos.y);
    }

    // glfwSetWindowIcon is not supported on Wayland (GLFW_FEATURE_UNAVAILABLE).
    // On Wayland the taskbar icon is provided by the .desktop file instead.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return;
    }

    auto logos = std::array{
        load_image("images/miximus_32x32.png"),
        load_image("images/miximus_64x64.png"),
        load_image("images/miximus_128x128.png"),
    };

    std::array<GLFWimage, logos.size()> glfw_logos{};
    std::ranges::transform(logos, glfw_logos.begin(), [](auto& logo) {
        return GLFWimage{
            .width  = logo.width(),
            .height = logo.height(),
            .pixels = logo.pixels().data(),
        };
    });

    glfwSetWindowIcon(window_, static_cast<int>(glfw_logos.size()), glfw_logos.data());
}

window_s::window_s(const window_settings_s& settings)
{
    initialize_glfw();
    configure_window_hints();
    const auto target = resolve_window_target(settings);
    // Supply placement before mapping, then apply it again with the other window settings.
    // Reset these hints for fullscreen creation so prior window settings cannot leak.
    glfwWindowHint(GLFW_POSITION_X,
                   (target.mode != nullptr) ? static_cast<int>(GLFW_ANY_POSITION) : settings.rect.pos.x);
    glfwWindowHint(GLFW_POSITION_Y,
                   (target.mode != nullptr) ? static_cast<int>(GLFW_ANY_POSITION) : settings.rect.pos.y);
    window_ = glfwCreateWindow(target.dimensions.x,
                               target.dimensions.y,
                               "Miximus",
                               (target.mode != nullptr) ? target.monitor : nullptr,
                               nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW Vulkan window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* window, int width, int height) {
        auto* self = static_cast<window_s*>(glfwGetWindowUserPointer(window));
        self->dimensions_ =
            (uint64_t{static_cast<uint32_t>(std::max(0, width))} << 32) | static_cast<uint32_t>(std::max(0, height));
    });
    int width{};
    int height{};
    glfwGetFramebufferSize(window_, &width, &height);
    dimensions_ = (uint64_t{static_cast<uint32_t>(width)} << 32) | static_cast<uint32_t>(height);
    try {
        configure_visible_window(settings, target);
    } catch (...) {
        glfwDestroyWindow(window_);
        throw;
    }
}

window_s::~window_s()
{
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
}

vec2i_t window_s::get_framebuffer_size()
{
    const auto size = dimensions_.load();
    return {static_cast<int>(size >> 32), static_cast<int>(size & UINT32_MAX)};
}

recti_s window_s::get_window_rect()
{
    recti_s result{};
    if (window_ == nullptr) {
        return result;
    }

    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        glfwGetWindowPos(window_, &result.pos.x, &result.pos.y);
    }

    glfwGetWindowSize(window_, &result.size.x, &result.size.y);
    return result;
}

void window_s::poll() { glfwPollEvents(); }
void window_s::terminate() { glfwTerminate(); }

window_system_s::window_system_s() { window_s::initialize_glfw(); }
window_system_s::~window_system_s() { window_s::terminate(); }

} // namespace miximus::gpu
