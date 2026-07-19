#ifdef _WIN32

#include "gpu/glad.hpp"
#include "monitor_platform.hpp"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <string>
#include <string_view>

namespace miximus::gpu::detail {

std::string get_monitor_id(GLFWmonitor* monitor)
{
    const auto* adapter_name = glfwGetWin32Adapter(monitor);
    const auto* monitor_name = glfwGetWin32Monitor(monitor);
    if (adapter_name != nullptr && monitor_name != nullptr) {
        for (DWORD index = 0;; ++index) {
            DISPLAY_DEVICEA device{};
            device.cb = static_cast<DWORD>(sizeof(device));
            if (::EnumDisplayDevicesA(adapter_name, index, &device, EDD_GET_DEVICE_INTERFACE_NAME) == FALSE) {
                break;
            }
            if (std::string_view(device.DeviceName) == monitor_name && device.DeviceID[0] != '\0') {
                return device.DeviceID;
            }
        }
    }

    return monitor_name != nullptr ? monitor_name : std::string{};
}

} // namespace miximus::gpu::detail

#endif // _WIN32
