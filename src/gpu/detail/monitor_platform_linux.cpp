#ifdef __linux__

#include "monitor_platform.hpp"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <X11/extensions/Xrandr.h>
#include <cstddef>
#include <string>

namespace miximus::gpu::detail {

std::string get_monitor_id(GLFWmonitor* monitor)
{
    auto* display = glfwGetX11Display();
    if (display != nullptr) {
        const auto output    = glfwGetX11Monitor(monitor);
        auto*      resources = XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));
        if (resources != nullptr) {
            auto* output_info = XRRGetOutputInfo(display, resources, output);
            if (output_info != nullptr) {
                std::string id(output_info->name, static_cast<std::size_t>(output_info->nameLen));
                XRRFreeOutputInfo(output_info);
                XRRFreeScreenResources(resources);
                return id;
            }
            XRRFreeScreenResources(resources);
        }
    }

    const auto* name = glfwGetMonitorName(monitor);
    return name != nullptr ? name : std::string{};
}

} // namespace miximus::gpu::detail

#endif // __linux__
