#include "context.hpp"
#include "context_logging.hpp"
#include "glad.hpp"
#include "logger/logger.hpp"
#include "shader.hpp"
#include "static_files/files.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "stb_image.h"
#include <frozen/map.h>

namespace {
using namespace miximus;

constexpr int GL_VERSION_MAJOR   = 4;
constexpr int GL_VERSION_MINOR   = 6;
constexpr int DEFAULT_CTX_WIDTH  = 640;
constexpr int DEFAULT_CTX_HEIGHT = 480;

const auto _log = [] { return getlog("gpu"); };

void monitor_config_callback(GLFWmonitor* monitor, int event)
{
    using namespace miximus::gpu;
    const auto name = glfwGetMonitorName(monitor);

    if (event == GLFW_CONNECTED) {
        context_s::monitors_g.emplace(name, monitor);
        _log()->info("Monitor connected: {}", name);
    } else {
        context_s::monitors_g.erase(name);
        _log()->info("Monitor disconnected: {}", name);
    }
}

GLFWimage load_image(std::string_view filename)
{
    const auto file_data = static_files::get_resource_files().get_file_or_throw(filename).unzip();

    GLFWimage img    = {};
    int       res_ch = 0;
    img.pixels       = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file_data.data()),
                                       static_cast<int>(file_data.size()),
                                       &img.width,
                                       &img.height,
                                       &res_ch,
                                       4);

    if (img.pixels == nullptr) {
        throw std::runtime_error(fmt::format("Failed to load logo {}", filename));
    }

    if (res_ch != 4) {
        stbi_image_free(img.pixels);
        throw std::runtime_error(fmt::format("Logo {} is not RGBA", filename));
    }

    return img;
}

} // namespace

namespace miximus::gpu {

context_s::context_s(bool visible, context_s* parent)
{
    static std::once_flag glfw_init;

    std::call_once(glfw_init, []() {
        _log()->debug("Initializing GLFW");
        if (glfwInit() == GLFW_FALSE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwSetErrorCallback(glfw_error_callback);
        glfwSetMonitorCallback(monitor_config_callback);

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
            auto       monitor = monitors[i];
            const auto name    = glfwGetMonitorName(monitor);

            _log()->info("Found monitor: {}", name);

            monitors_g.emplace(name, monitor);
        }
    });

    const int visible_flag = visible ? GLFW_TRUE : GLFW_FALSE;
    glfwWindowHint(GLFW_SRGB_CAPABLE, visible_flag);
    glfwWindowHint(GLFW_VISIBLE, visible_flag);
    glfwWindowHint(GLFW_DOUBLEBUFFER, visible_flag);

    auto parent_window = (parent != nullptr) ? parent->window_ : nullptr;
    window_            = glfwCreateWindow(DEFAULT_CTX_WIDTH, DEFAULT_CTX_HEIGHT, "Miximus", nullptr, parent_window);

    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);

    make_current();

    static std::once_flag glad_init;
    std::call_once(glad_init, []() {
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            throw std::runtime_error("GLAD failed to load OpenGL procs");
        }

        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(opengl_error_callback, nullptr);
    });

    if (visible) {
        glfwSwapInterval(1);

        const auto logos = std::array{
            load_image("images/miximus_32x32.png"),
            load_image("images/miximus_64x64.png"),
            load_image("images/miximus_128x128.png"),
        };

        glfwSetWindowIcon(window_, logos.size(), logos.data());

        for (const auto& logo : logos) {
            stbi_image_free(logo.pixels);
        }
    } else {
        // glfwSwapInterval(0);
    }

    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnablei(GL_BLEND, 0);
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_FRAMEBUFFER_SRGB);

    rewind_current();
}

context_s::~context_s()
{
    if (window_ != nullptr) {
        make_current();
        shaders_.clear();
        rewind_current();
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

void context_s::set_fullscreen_monitor(const std::string& name, recti_s rect)
{
    auto it = monitors_g.find(name);
    if (it != monitors_g.end()) {
        if (it->second == glfwGetWindowMonitor(window_)) {
            return;
        }

        const auto mode = glfwGetVideoMode(it->second);
        glfwSetWindowMonitor(window_, it->second, 0, 0, mode->width, mode->height, GLFW_DONT_CARE);

    } else {
        glfwSetWindowMonitor(window_, nullptr, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y, GLFW_DONT_CARE);
    }
}

void context_s::make_current()
{
    GLFWwindow* old = nullptr;
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

std::shared_ptr<context_s> context_s::create_shared_context(bool visible, context_s* parent)
{
    return std::make_shared<context_s>(visible, parent);
}

} // namespace miximus::gpu
