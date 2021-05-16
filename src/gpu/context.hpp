#pragma once
struct GLFWwindow;

namespace miximus::gpu {
class context
{
    GLFWwindow* window_;

  public:
    context(bool visible = false, context* parent = nullptr);
    ~context();

    void        make_current();
    static void make_null_current();

    static void poll();
    static void terminate();
};

} // namespace miximus::gpu
