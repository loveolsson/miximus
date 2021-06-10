#include "gpu/context.hpp"
#include "logger/logger.hpp"

#include <glad/glad.h>
// GLAD needs to be included before GLFW to avoid APIENTRY redefinition warning in MSVC
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <mutex>

namespace miximus::gpu {

static std::once_flag glfw_init, glad_init;

void error_callback(int error, const char* description) { spdlog::get("gpu")->error("Error: {}", description); }

context::context(bool visible, context* parent)
    : window_(nullptr)
{
    std::call_once(glfw_init, []() {
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwSetErrorCallback(error_callback);

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    });

    if (visible) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    } else {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    GLFWwindow* parent_window = nullptr;
    if (parent) {
        parent_window = parent->window_;
    }

    window_ = glfwCreateWindow(640, 480, "Miximus", NULL, parent_window);
    if (!window_) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    make_current();

    std::call_once(glad_init, []() {
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            throw std::runtime_error("GLAD failed to load OpenGL procs");
        }
    });

    if (visible) {
        glfwSwapInterval(1);
    }
}

context::~context()
{
    if (window_) {
        glfwDestroyWindow(window_);
    }
}

void context::make_current() { glfwMakeContextCurrent(window_); }

void context::make_null_current() { glfwMakeContextCurrent(nullptr); }

void context::poll() { glfwPollEvents(); }

void context::terminate() { glfwTerminate(); }

} // namespace miximus::gpu
