#pragma once
#include "shader.hpp"
#include "types.hpp"
#include "types/settings_option.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;
struct GLFWmonitor;
struct GLFWvidmode;

namespace miximus::gpu {

class context_scope_s;

class context_s
{
    friend class context_scope_s;

  public:
    struct window_settings_s
    {
        bool             visible{};
        bool             fullscreen{};
        std::string_view monitor_id{};
        recti_s          rect{
                     {0,   0  },
                     {640, 480}
        };
    };

  private:
    using shader_map_t = std::map<shader_program_s::name_e, std::unique_ptr<shader_program_s>>;

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

    static inline thread_local std::vector<GLFWwindow*>                current_stack_;
    static inline std::atomic<uint64_t>                                monitor_list_version_{0};
    static inline std::map<std::string, monitor_record_s, std::less<>> monitors_;

    GLFWwindow*  window_{};
    shader_map_t shaders_;
    void         make_current();
    static void  rewind_current();
    static void  monitor_config_callback(GLFWmonitor* monitor, int event) noexcept;
    static void  initialize_glfw();
    static void  initialize_glad();
    static void  configure_window_hints(bool visible);
    static auto  resolve_window_target(const window_settings_s& settings) -> window_target_s;
    void         configure_visible_window(const window_settings_s& settings, const window_target_s& target);

  public:
    context_s(bool visible, context_s* parent);
    context_s(const window_settings_s& settings, context_s* parent);
    ~context_s();

    vec2i_t get_framebuffer_size();
    recti_s get_window_rect();

    static bool has_current() noexcept;
    static bool require_current();

    void swap_buffers();

    static void                           finish();
    static void                           flush();
    static void                           poll();
    static void                           terminate();
    static bool                           has_extension(const char* ext);
    static uint64_t                       get_monitor_list_version() noexcept;
    static std::vector<settings_option_s> get_monitors();
    static std::optional<int>             get_monitor_refresh_rate(std::string_view monitor_id);

    shader_program_s* get_shader(shader_program_s::name_e name);

    static std::unique_ptr<context_s> create_unique_context(bool visible = false, context_s* parent = nullptr);
    static std::unique_ptr<context_s> create_unique_context(const window_settings_s& settings,
                                                            context_s*               parent = nullptr);
};

class context_scope_s
{
  public:
    explicit context_scope_s(context_s& context);
    ~context_scope_s();

    context_scope_s(const context_scope_s&)            = delete;
    context_scope_s(context_scope_s&&)                 = delete;
    context_scope_s& operator=(const context_scope_s&) = delete;
    context_scope_s& operator=(context_scope_s&&)      = delete;
};

} // namespace miximus::gpu
