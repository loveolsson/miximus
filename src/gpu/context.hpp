#pragma once
#include "shader.hpp"
#include "types.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

struct GLFWwindow;
struct GLFWmonitor;

namespace miximus::gpu {

class context_s
{
    using shader_map_t = std::map<shader_program_s::name_e, std::unique_ptr<shader_program_s>>;

    static inline thread_local std::vector<GLFWwindow*> current_stack_;

    GLFWwindow*  window_{};
    shader_map_t shaders_;
    std::mutex   mtx_;

  public:
    context_s(bool visible, context_s* parent);
    ~context_s();

    vec2i_t get_framebuffer_size();
    recti_s get_window_rect();

    void set_window_rect(recti_s rect);
    void set_fullscreen_monitor(const std::string& name, recti_s rect);

    void        make_current();
    static void rewind_current();
    static bool has_current();

    void swap_buffers();

    static void finish();
    static void flush();
    static void poll();
    static void terminate();
    static bool has_extension(const char* ext);

    auto get_lock() { return std::unique_lock(mtx_); };

    shader_program_s* get_shader(shader_program_s::name_e name);

    static std::unique_ptr<context_s> create_unique_context(bool visible = false, context_s* parent = nullptr);
    static std::shared_ptr<context_s> create_shared_context(bool visible = false, context_s* parent = nullptr);

    static inline std::map<std::string, GLFWmonitor*> monitors_g;
};

} // namespace miximus::gpu
