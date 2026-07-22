#pragma once
#include "utils/flicks.hpp"

#include <string_view>

namespace miximus::core {

class clock_source_i
{
  public:
    clock_source_i()          = default;
    virtual ~clock_source_i() = default;

    clock_source_i(const clock_source_i&)            = delete;
    clock_source_i(clock_source_i&&)                 = delete;
    clock_source_i& operator=(const clock_source_i&) = delete;
    clock_source_i& operator=(clock_source_i&&)      = delete;

    virtual utils::flicks    now() const                    = 0;
    virtual void             wait_until(utils::flicks time) = 0;
    virtual std::string_view name() const                   = 0;
};

class steady_clock_source_s final : public clock_source_i
{
  public:
    utils::flicks    now() const final;
    void             wait_until(utils::flicks time) final;
    std::string_view name() const final;
};

} // namespace miximus::core
