#pragma once
#include "interface_type.hpp"
#include "nodes/connection.hpp"

#include <optional>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

class node_cfg;

class interface
{
    std::unordered_set<connection> connections_;
    bool                           is_input_;
    bool                           single_connection_;

  public:
    interface(bool is_input, bool single_connection)
        : is_input_(is_input)
        , single_connection_(single_connection)
    {
    }

    virtual ~interface() {}

    bool                    add_connection(const connection& con, std::vector<connection>& removed);
    bool                    remove_connection(const connection& con);
    std::vector<interface*> resolve_connections(const node_cfg& cfg);
    interface*              resolve_connection(const node_cfg& cfg);
    bool                    is_input() { return is_input_; }

    virtual interface_type_e type()            = 0;
    virtual bool             has_value() const = 0;
    virtual void             reset()           = 0;
};

template <typename T>
class interface_typed : public interface
{
    std::optional<T> value_;

  public:
    interface_typed(bool is_input, bool single_connection)
        : interface(is_input, single_connection){};
    ~interface_typed(){};

    std::vector<T>   resolve_connection_values(const node_cfg& cfg);
    std::optional<T> resolve_connection_value(const node_cfg& cfg);

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
std::vector<T> interface_typed<T>::resolve_connection_values(const node_cfg& cfg)
{
    std::vector<T> result;

    for (auto iface : resolve_connections(cfg)) {
        result.emplace_back(get_value_from(iface));
    }

    return result;
}

template <typename T>
std::optional<T> interface_typed<T>::resolve_connection_value(const node_cfg& cfg)
{
    if (auto iface = resolve_connection(cfg)) {
        return get_value_from(iface);
    }

    return std::nullopt;
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

    auto typed = static_cast<interface_typed<T>*>(iface);
    return typed->get_value();
}

template <>
double interface_typed<double>::get_value_from(interface* iface);

template <>
int64_t interface_typed<int64_t>::get_value_from(interface* iface);

} // namespace miximus::nodes