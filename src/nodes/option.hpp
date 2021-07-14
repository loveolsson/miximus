#pragma once
#include "gpu/types.hpp"

#include <nlohmann/json_fwd.hpp>

#include <mutex>
#include <set>
#include <string>

namespace miximus::nodes {

class option_i
{
  protected:
    mutable std::mutex value_mutex_;

  public:
    virtual ~option_i() = default;

    virtual bool           set_json(const nlohmann::json&) = 0;
    virtual nlohmann::json get_json() const                = 0;
};

class option_name : public option_i
{
    std::string                         name_;
    inline static std::mutex            names_in_use_mutex_;
    inline static std::set<std::string> names_in_use;

  public:
    option_name() {}
    ~option_name();

    bool           set_json(const nlohmann::json&) final;
    nlohmann::json get_json() const final;
    std::string    get_value() const;
};

class option_position : public option_i
{
    gpu::vec2 pos_;

  public:
    option_position()
        : pos_({0, 0})
    {
    }

    ~option_position() {}

    bool           set_json(const nlohmann::json&) final;
    nlohmann::json get_json() const final;
    gpu::vec2      get_value() const;
};

} // namespace miximus::nodes