#pragma once
#include "nodes/option.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {

template <typename T>
using option_set_f = bool (*)(T&, const nlohmann::json&);

template <typename T>
using option_get_f = nlohmann::json (*)(const T&);

template <typename T>
inline bool default_option_setter(T& t, const nlohmann::json& j)
{
    try {
        t = j;
        return true;
    } catch (nlohmann::json::exception& e) {
        return false;
    }
}

template <typename T>
inline nlohmann::json default_option_getter(const T& t)
{
    return t;
}

template <typename T, option_set_f<T> Set = default_option_setter, option_get_f<T> Get = default_option_getter>
class option_typed : public option
{
    T v_;

  public:
    option_typed()
        : v_(T())
    {
    }

    bool           set_json(const nlohmann::json& j) final { return Set(v_, j); }
    nlohmann::json get_json() const final { return Get(v_); }
    const T&       get_value() { return v_; }
};

} // namespace miximus::nodes