#pragma once
#include "nodes/option.hpp"

#include <nlohmann/json.hpp>

#include <functional>

namespace miximus::nodes {

template <typename T>
using option_set_f = std::function<bool(T&, const nlohmann::json&)>;

template <typename T>
using option_get_f = std::function<nlohmann::json(const T&)>;

template <typename T>
auto default_option_setter = [](T& t, const nlohmann::json& j) -> bool {
    try {
        t = j;
        return true;
    } catch (nlohmann::json::exception& e) {
        return false;
    }
};

template <typename T>
auto default_option_getter = [](const T& t) -> nlohmann::json { return t; };

template <typename T>
class option_typed : public option
{
    T               v_;
    option_set_f<T> setter_;
    option_get_f<T> getter_;

  public:
    option_typed(option_set_f<T> s = default_option_setter<T>, option_get_f<T> g = default_option_getter<T>)
        : v_(T())
        , setter_(s)
        , getter_(g)
    {
    }

    bool           set_json(const nlohmann::json& j) final { return setter_(v_, j); }
    nlohmann::json get_json() const final { return getter_(v_); }
    const T&       get_value() { return v_; }
};

} // namespace miximus::nodes