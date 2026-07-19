#ifdef __APPLE__

#include "monitor_platform.hpp"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <ColorSync/ColorSync.h>
#include <CoreFoundation/CoreFoundation.h>
#include <array>
#include <string>

namespace miximus::gpu::detail {

std::string get_monitor_id(GLFWmonitor* monitor)
{
    const auto display_id = glfwGetCocoaMonitor(monitor);
    auto*      uuid       = CGDisplayCreateUUIDFromDisplayID(display_id);
    if (uuid == nullptr) {
        return std::to_string(display_id);
    }

    auto* uuid_string = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    CFRelease(uuid);
    if (uuid_string == nullptr) {
        return std::to_string(display_id);
    }

    std::array<char, 64> buffer{};
    const bool           converted =
        CFStringGetCString(uuid_string, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8);
    CFRelease(uuid_string);
    return converted ? std::string(buffer.data()) : std::to_string(display_id);
}

} // namespace miximus::gpu::detail

#endif // __APPLE__
