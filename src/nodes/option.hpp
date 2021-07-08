#pragma once
#include <nlohmann/json_fwd.hpp>

#include <set>
#include <string>

namespace miximus::nodes {

class option
{
  public:
    virtual ~option() {}

    virtual bool           set_json(const nlohmann::json&) = 0;
    virtual nlohmann::json get_json() const                = 0;
};

class option_name : public option
{
    std::string                         name_;
    inline static std::set<std::string> names_in_use;

  public:
    option_name() {}
    ~option_name();

    bool           set_json(const nlohmann::json&) final;
    nlohmann::json get_json() const final;
};

class option_position : public option
{
    double x_, y_;

  public:
    option_position()
        : x_(0)
        , y_(0)
    {
    }

    ~option_position() {}

    bool           set_json(const nlohmann::json&) final;
    nlohmann::json get_json() const final;
};

} // namespace miximus::nodes