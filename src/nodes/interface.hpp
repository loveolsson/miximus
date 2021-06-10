#pragma once
#include "interface_type.hpp"
#include "nodes/connection.hpp"

#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

class node_cfg;

class interface
{
    std::unordered_set<connection> connections_;
    bool                           is_input_;
    int                            max_connection_count_;

  public:
    interface(bool is_input)
        : is_input_(is_input)
        , max_connection_count_(is_input_ ? 1 : INT_MAX)
    {
    }

    virtual ~interface() {}

    bool       add_connection(const connection& con, std::vector<connection>& removed);
    bool       remove_connection(const connection& con);
    interface* resolve_connection(const node_cfg& cfg);
    bool       is_input() { return is_input_; }
    void       set_max_connections(int count) { max_connection_count_ = count; }

    virtual interface_type_e type()            = 0;
    virtual bool             has_value() const = 0;
    virtual void             reset()           = 0;
};

template <typename T>
class interface_typed : public interface
{
    std::optional<T> value_;

  public:
    interface_typed(bool is_input)
        : interface(is_input){};
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

    /**
     * Get value from another interface, with conversion if possible
     * @throw std::runtime_error if the value conversion is not possible
     */
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
        throw std::runtime_error("incompatible interface types");
    }

    return static_cast<interface_typed<T>*>(iface)->get_value();
}

template <>
double interface_typed<double>::get_value_from(interface* iface);

template <>
int64_t interface_typed<int64_t>::get_value_from(interface* iface);

} // namespace miximus::nodes