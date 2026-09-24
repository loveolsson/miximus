#pragma once

#include "include/cef_task.h"

#include <functional>

namespace miximus::nodes::cef::detail {

class task_s final : public CefTask
{
    std::function<void()> function_;
    IMPLEMENT_REFCOUNTING(task_s);

  public:
    explicit task_s(std::function<void()> function)
        : function_(std::move(function))
    {
    }
    void Execute() override { function_(); }
};

} // namespace miximus::nodes::cef::detail
