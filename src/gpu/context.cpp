#include "context.hpp"

#include "context_logging.hpp"
#include "detail/monitor_platform.hpp"
#include "glad.hpp"
#include "logger/logger.hpp"
#include "shader.hpp"
#include "static_files/files.hpp"
#include "wrapper/stb/image.hpp"

#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>
#include <frozen/map.h>

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

constexpr int GL_VERSION_MAJOR   = 4;
constexpr int GL_VERSION_MINOR   = 6;
constexpr int DEFAULT_CTX_WIDTH  = 640;
constexpr int DEFAULT_CTX_HEIGHT = 480;

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

void context_s::monitor_config_callback(GLFWmonitor* monitor, int event)
{
    bool changed = false;
    if (event == GLFW_CONNECTED) {
        const auto* raw_label = glfwGetMonitorName(monitor);
        const auto  id        = detail::get_monitor_id(monitor);
        const auto  label     = raw_label != nullptr ? std::string(raw_label) : id;
        changed               = monitors_.emplace(id, monitor_record_s{.label = label, .handle = monitor}).second;
        _log()->info("Monitor connected: {} ({})", label, id);
    } else {
        const auto it =
            std::ranges::find_if(monitors_, [monitor](const auto& entry) { return entry.second.handle == monitor; });
        if (it != monitors_.end()) {
            _log()->info("Monitor disconnected: {} ({})", it->second.label, it->first);
            monitors_.erase(it);
            changed = true;
        }
    }

    if (changed) {
        monitor_list_version_.fetch_add(1, std::memory_order_relaxed);
    }
}

context_scope_s::context_scope_s(context_s& context) { context.make_current(); }

context_scope_s::~context_scope_s() { context_s::rewind_current(); }

uint64_t context_s::get_monitor_list_version() { return monitor_list_version_.load(std::memory_order_relaxed); }

std::vector<settings_option_s> context_s::get_monitors()
{
    std::vector<settings_option_s> monitors;
    monitors.reserve(monitors_.size());
    for (const auto& [id, monitor] : monitors_) {
        monitors.push_back({.id = id, .label = monitor.label});
    }
    return monitors;
}

context_s::context_s(bool visible, context_s* parent)
{
    static std::once_flag glfw_init;

    std::call_once(glfw_init, []() {
        _log()->debug("Initializing GLFW");

#ifdef __linux__
        // Force X11/XWayland so we get a GLX context.
        // This enables DVP (GPU Direct), free borderless-window positioning, and
        // avoids EGL restrictions. Professional video hardware (DeckLink, Quadro)
        // requires GLX; the app runs as an XWayland client on Wayland desktops.
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

        if (glfwInit() == GLFW_FALSE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwSetErrorCallback(glfw_error_callback);
        glfwSetMonitorCallback(context_s::monitor_config_callback);

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_VERSION_MAJOR);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_VERSION_MINOR);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_SAMPLES, 4);
        // glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

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

    const int visible_flag = visible ? GLFW_TRUE : GLFW_FALSE;
    glfwWindowHint(GLFW_SRGB_CAPABLE, visible_flag);
    glfwWindowHint(GLFW_VISIBLE, visible_flag);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

    auto parent_window = (parent != nullptr) ? parent->window_ : nullptr;
    window_            = glfwCreateWindow(DEFAULT_CTX_WIDTH, DEFAULT_CTX_HEIGHT, "Miximus", nullptr, parent_window);

    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);

    const context_scope_s context_scope(*this);

    static std::once_flag glad_init;
    std::call_once(glad_init, []() {
        if (gladLoadGL(glfwGetProcAddress) == 0) {
            throw std::runtime_error("GLAD failed to load OpenGL procs");
        }

        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(opengl_error_callback, nullptr);
    });

    if (visible) {
        glfwSwapInterval(1);

        // glfwSetWindowIcon is not supported on Wayland (GLFW_FEATURE_UNAVAILABLE).
        // On Wayland the taskbar icon is provided by the .desktop file instead.
        if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
            auto logos = std::array{
                load_image("images/miximus_32x32.png"),
                load_image("images/miximus_64x64.png"),
                load_image("images/miximus_128x128.png"),
            };
            std::array<GLFWimage, 3> glfw_logos{};
            std::ranges::transform(logos, glfw_logos.begin(), [](auto& logo) {
                return GLFWimage{
                    .width  = logo.width(),
                    .height = logo.height(),
                    .pixels = logo.pixels().data(),
                };
            });

            glfwSetWindowIcon(window_, static_cast<int>(glfw_logos.size()), glfw_logos.data());
        }
    }

    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnablei(GL_BLEND, 0);
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_FRAMEBUFFER_SRGB);
}

context_s::~context_s()
{
    if (window_ != nullptr) {
        {
            const context_scope_s context_scope(*this);
            shaders_.clear();
        }
        glfwDestroyWindow(window_);
    }
}

vec2i_t context_s::get_framebuffer_size()
{
    vec2i_t res;
    glfwGetFramebufferSize(window_, &res.x, &res.y);
    return res;
}

recti_s context_s::get_window_rect()
{
    recti_s res{};
    glfwGetWindowPos(window_, &res.pos.x, &res.pos.y);
    glfwGetWindowSize(window_, &res.size.x, &res.size.y);
    return res;
}

void context_s::set_window_rect(recti_s rect)
{
    glfwSetWindowMonitor(window_, nullptr, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y, GLFW_DONT_CARE);
}

void context_s::set_fullscreen_monitor(std::string_view monitor_id, recti_s rect)
{
    auto it = monitors_.find(monitor_id);
    if (it == monitors_.end()) {
        // Compatibility for settings saved before monitor IDs and labels were separated.
        it = std::ranges::find_if(monitors_,
                                  [monitor_id](const auto& entry) { return entry.second.label == monitor_id; });
    }

    if (it != monitors_.end()) {
        auto* monitor = it->second.handle;
        if (monitor == glfwGetWindowMonitor(window_)) {
            return;
        }

        const auto mode = glfwGetVideoMode(monitor);
        if (mode == nullptr) {
            _log()->error("Unable to query video mode for monitor '{}'", monitor_id);
            glfwSetWindowMonitor(window_, nullptr, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y, GLFW_DONT_CARE);
            return;
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            // Mutter under XWayland may choose the output containing the window
            // instead of honoring _NET_WM_FULLSCREEN_MONITORS. Move the window
            // onto the requested output before asking the WM to fullscreen it.
            int monitor_x{};
            int monitor_y{};
            glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
            glfwSetWindowMonitor(window_, nullptr, monitor_x, monitor_y, mode->width, mode->height, GLFW_DONT_CARE);
        }

        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);

    } else {
        glfwSetWindowMonitor(window_, nullptr, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y, GLFW_DONT_CARE);
    }
}

void context_s::make_current()
{
    const GLFWwindow* old = nullptr;
    if (!current_stack_.empty()) {
        old = current_stack_.back();
    }

    if (old != window_) {
        glfwMakeContextCurrent(window_);
    }

    current_stack_.push_back(window_);
}

void context_s::rewind_current()
{
    assert(!current_stack_.empty());

    if (current_stack_.empty()) {
        return;
    }

    if (current_stack_.size() == 1) {
        glfwMakeContextCurrent(nullptr);
    } else {
        GLFWwindow* prior = current_stack_[current_stack_.size() - 2];

        if (prior != current_stack_[current_stack_.size() - 1]) {
            glfwMakeContextCurrent(prior);
        }
    }

    current_stack_.pop_back();
}

bool context_s::has_current() { return !current_stack_.empty(); }

bool context_s::require_current()
{
    const bool ok = !current_stack_.empty();
    assert(ok);
    return ok;
}

void context_s::swap_buffers() { glfwSwapBuffers(window_); }

void context_s::finish() { glFinish(); }

void context_s::flush() { glFlush(); }

void context_s::poll() { glfwPollEvents(); }

void context_s::terminate() { glfwTerminate(); }

bool context_s::has_extension(const char* ext) { return glfwExtensionSupported(ext) != 0; }

shader_program_s* context_s::get_shader(shader_program_s::name_e name)
{
    using name_e = shader_program_s::name_e;

    if (auto it = shaders_.find(name); it != shaders_.end()) {
        return it->second.get();
    }

    constexpr auto shaderInfo = frozen::make_map<name_e, std::pair<std::string_view, std::string_view>>({
        {name_e::basic,       {"shaders/basic.vs.glsl", "shaders/basic.fs.glsl"}      },
        {name_e::yuv_to_rgb,  {"shaders/basic.vs.glsl", "shaders/from_yuv.fs.glsl"}   },
        {name_e::rgb_to_yuv,  {"shaders/basic.vs.glsl", "shaders/to_yuv.fs.glsl"}     },
        {name_e::apply_gamma, {"shaders/basic.vs.glsl", "shaders/apply_gamma.fs.glsl"}},
        {name_e::strip_gamma, {"shaders/basic.vs.glsl", "shaders/strip_gamma.fs.glsl"}},
    });

    const auto& info = shaderInfo.at(name);

    auto [it, _] = shaders_.emplace(name, std::make_unique<shader_program_s>(info.first, info.second));

    return it->second.get();
}

std::unique_ptr<context_s> context_s::create_unique_context(bool visible, context_s* parent)
{
    return std::make_unique<context_s>(visible, parent);
}

} // namespace miximus::gpu
