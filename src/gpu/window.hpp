#pragma once

#include "types.hpp"
#include "types/settings_option.hpp"
#include "utils/string_map.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;
struct GLFWmonitor;
struct GLFWvidmode;

namespace miximus::gpu {

class window_s
{
  public:
    struct window_settings_s
    {
        bool             fullscreen{};
        std::string_view monitor_id{};
        recti_s          rect{
                     {0,   0  },
                     {640, 480}
        };
    };

  private:
    struct monitor_record_s
    {
        std::string  label;
        GLFWmonitor* handle{};
    };

    struct window_target_s
    {
        GLFWmonitor*       monitor{};
        const GLFWvidmode* mode{};
        vec2i_t            dimensions{};
    };

    static inline std::atomic<uint64_t>                 monitor_list_version_{0};
    static inline utils::string_map_t<monitor_record_s> monitors_;

    GLFWwindow*              window_{};
    std::atomic_uint64_t     dimensions_{};
    static inline std::mutex monitor_mutex_;
    static void              monitor_config_callback(GLFWmonitor* monitor, int event) noexcept;
    static void              initialize_glfw();
    static void              configure_window_hints();
    static auto              resolve_window_target(const window_settings_s& settings) -> window_target_s;
    void                     configure_visible_window(const window_settings_s& settings, const window_target_s& target);

  public:
    explicit window_s(const window_settings_s& settings);
    window_s(const window_s&)            = delete;
    window_s& operator=(const window_s&) = delete;
    ~window_s();

    vec2i_t get_framebuffer_size();
    recti_s get_window_rect();

    static void                           poll();
    static void                           terminate();
    static uint64_t                       get_monitor_list_version() noexcept;
    static std::vector<settings_option_s> get_monitors();
    static std::optional<int>             get_monitor_refresh_rate(std::string_view monitor_id);

    GLFWwindow* native_window() const noexcept { return window_; }

    friend class window_system_s;
};

// Owns GLFW initialization/termination, independently of visible windows.
class window_system_s
{
  public:
    window_system_s();
    ~window_system_s();
    window_system_s(const window_system_s&)            = delete;
    window_system_s& operator=(const window_system_s&) = delete;
};
} // namespace miximus::gpu
