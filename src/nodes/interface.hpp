#pragma once
#include "interface_type.hpp"
#include "nodes/config/config.hpp"

#include <climits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

class interface
{
  public:
    enum class dir
    {
        input,
        output,
    };

  private:
    con_set_t connections_;
    dir       direction_;
    int       max_connection_count_;

  public:
    interface(dir direction)
        : direction_(direction)
        , max_connection_count_(direction == dir::input ? 1 : INT_MAX)
    {
    }

    virtual ~interface() {}

    bool             add_connection(const connection& con, con_set_t& removed);
    bool             remove_connection(const connection& con);
    interface*       resolve_connection(const node_cfg& cfg);
    const con_set_t& get_connections() { return connections_; }
    dir              direction() { return direction_; }
    void             set_max_connections(int count) { max_connection_count_ = count; }

    virtual interface_type_e type()            = 0;
    virtual bool             has_value() const = 0;
    virtual void             reset()           = 0;
};

template <typename T>
class interface_typed : public interface
{
    std::optional<T> value_;

  public:
    interface_typed(dir direction)
        : interface(direction){};
    ~interface_typed(){};

    bool resolve_connection_value(const node_cfg& cfg);

    interface_type_e type() final { return get_interface_type<T>(); }
    bool             has_value() const final { return value_ != std::nullopt; }
    void             reset() final { value_ = std::nullopt; }

    const T get_value() const
    {
        if (!has_value()) {
            throw std::runtime_error("get_value called without a value set");
        }

        return *value_;
    }

    void set_value(const T& value) { value_ = value; }

    static T get_value_from(interface*);
};

template <typename T>
bool interface_typed<T>::resolve_connection_value(const node_cfg& cfg)
{
    if (auto iface = resolve_connection(cfg)) {
        value_ = get_value_from(iface);
        return true;
    }

    value_ = T();
    return false;
}

/**
 * Implementations of get_value_from
 */

template <typename T>
T interface_typed<T>::get_value_from(interface* iface)
{
    if (get_interface_type<T>() != iface->type()) {
        return T();
    }

    return static_cast<interface_typed<T>*>(iface)->get_value();
}

template <>
double interface_typed<double>::get_value_from(interface* iface);

template <>
int64_t interface_typed<int64_t>::get_value_from(interface* iface);

} // namespace miximus::nodes