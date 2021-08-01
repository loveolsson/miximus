#include "context.hpp"
#include "glad.hpp"
#include "logger/logger.hpp"
#include "shader.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace miximus::gpu {

constexpr int GL_VERSION_MAJOR   = 4;
constexpr int GL_VERSION_MINOR   = 5;
constexpr int DEFAULT_CTX_WIDTH  = 640;
constexpr int DEFAULT_CTX_HEIGHT = 480;

static std::once_flag                      glfw_init, glad_init;
static std::map<std::string, GLFWmonitor*> monitors_g;

static void error_callback(int error, const char* description)
{
    getlog("gpu")->error("Error: [{}]:{}", error, description); //
}

void GLAPIENTRY opengl_error_callback(GLenum source,
                                      GLenum type,
                                      GLuint /*id*/,
                                      GLenum        severity,
                                      GLsizei       length,
                                      const GLchar* message,
                                      const void* /*userParam*/)
{
    std::string_view source_str;
    std::string_view type_str;
    std::string_view severity_str;

    switch (source) {
        case GL_DEBUG_SOURCE_API:
            source_str = "API";
            break;
        case GL_DEBUG_SOURCE_OTHER:
        default:
            source_str = "Other";
            break;
    }

    switch (type) {
        case GL_DEBUG_TYPE_ERROR:
            type_str = "Error";
            break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            type_str = "Deprecated Behaviour";
            break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
            type_str = "Undefined Behaviour";
            break;
        case GL_DEBUG_TYPE_PORTABILITY:
            type_str = "Portability";
            break;
        case GL_DEBUG_TYPE_PERFORMANCE:
            type_str = "Performance";
            break;
        case GL_DEBUG_TYPE_MARKER:
            type_str = "Marker";
            break;
        case GL_DEBUG_TYPE_PUSH_GROUP:
            type_str = "Push Group";
            break;
        case GL_DEBUG_TYPE_POP_GROUP:
            type_str = "Pop Group";
            break;
        case GL_DEBUG_TYPE_OTHER:
        default:
            type_str = "Other";
            break;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:
            severity_str = "high";
            break;
        case GL_DEBUG_SEVERITY_MEDIUM:
            severity_str = "medium";
            break;
        case GL_DEBUG_SEVERITY_LOW:
            severity_str = "low";
            break;
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            severity_str = "notification";
            break;
        default:
            severity_str = "unknown";
            break;
    }

    if (type == GL_DEBUG_TYPE_ERROR) {
        getlog("gpu")->error("OpenGL error: source = {}, type = {}, severity = {}, message = {}",
                             source_str,
                             type_str,
                             severity_str,
                             std::string_view(message, length));

    } else {
        getlog("gpu")->warn("OpenGL error: source = {}, type = {}, severity = {}, message = {}",
                            source_str,
                            type_str,
                            severity_str,
                            std::string_view(message, length));
    }
}

static void monitor_config_callback(GLFWmonitor* monitor, int event)
{
    const auto* name = glfwGetMonitorName(monitor);
    if (event == GLFW_CONNECTED) {
        monitors_g.emplace(name, monitor);
    } else {
        monitors_g.erase(name);
    }
}

context_s::context_s(bool visible, context_s* parent)
{
    std::call_once(glfw_init, []() {
        getlog("gpu")->debug("Initializing GLFW");
        if (glfwInit() == GLFW_FALSE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwSetErrorCallback(error_callback);
        glfwSetMonitorCallback(monitor_config_callback);

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_VERSION_MAJOR);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_VERSION_MINOR);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        // glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        int   count{};
        auto* monitors = glfwGetMonitors(&count);
        for (int i = 0; i < count; ++i) {
            auto*       monitor = monitors[i];
            const auto* name    = glfwGetMonitorName(monitor);

            getlog("gpu")->info("Found monitor: {}", name);

            monitors_g.emplace(name, monitor);
        }
    });

    if (visible) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    } else {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    GLFWwindow* parent_window = nullptr;
    if (parent != nullptr) {
        parent_window = parent->window_;
    }

    window_ = glfwCreateWindow(DEFAULT_CTX_WIDTH, DEFAULT_CTX_HEIGHT, "Miximus", nullptr, parent_window);
    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);

    make_current();

    std::call_once(glad_init, []() {
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            throw std::runtime_error("GLAD failed to load OpenGL procs");
        }

        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(opengl_error_callback, nullptr);
    });

    if (visible) {
        glfwSwapInterval(0);
    }

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

        const auto* mode = glfwGetVideoMode(it->second);
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

        if (prior != current_stack_.back()) {
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

shader_program_s* context_s::get_shader(shader_program_s::name_e name)
{
    using name_e = shader_program_s::name_e;

    auto it = shaders_.find(name);
    if (it != shaders_.end()) {
        return it->second.get();
    }

    std::unique_ptr<shader_program_s> shader;

    switch (name) {
        case name_e::basic:
            shader = std::make_unique<shader_program_s>("basic_vert.glsl", "basic_frag.glsl");
            break;
        case name_e::yuv_to_rgb:
            shader = std::make_unique<shader_program_s>("basic_vert.glsl", "from_yuv_frag.glsl");
            break;

        default:
            throw std::runtime_error("Trying to create unknown shader type");
            break;
    }

    auto [new_it, _] = shaders_.emplace(name, std::move(shader));
    return new_it->second.get();
}

} // namespace miximus::gpu
