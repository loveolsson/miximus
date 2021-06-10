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
    bool                           single_connection_;

  public:
    interface(bool single_connection)
        : single_connection_(single_connection)
    {
    }
    virtual ~interface() {}

    bool                    add_connection(const connection& con, std::vector<connection>& removed);
    bool                    remove_connection(const connection& con);
    std::vector<interface*> resolve_connections(const node_cfg& cfg);

    virtual bool             is_input()        = 0;
    virtual interface_type_e type()            = 0;
    virtual bool             has_value() const = 0;
    virtual void             reset()           = 0;
};

template <typename T, bool IsInput>
class interface_typed : public interface
{
    std::optional<T> value_;

  public:
    interface_typed(bool single_connection = false)
        : interface(single_connection){};
    ~interface_typed(){};

    std::vector<T> resolve_connection_values(const node_cfg& cfg);

    bool             is_input() final { return IsInput; }
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

template <typename T, bool IsInput>
T interface_typed<T, IsInput>::get_value_from(interface* iface)
{
    static_assert(IsInput, "get_value_from used from output");

    if (get_interface_type<T>() != iface->type()) {
        throw std::runtime_error("incompatible interface types");
    }

    auto typed = static_cast<interface_typed<T, false>*>(iface);
    return typed->get_value();
}

template <>
double interface_typed<double, false>::get_value_from(interface* iface);

template <>
int64_t interface_typed<int64_t, false>::get_value_from(interface* iface);

template <typename T, bool IsInput>
std::vector<T> interface_typed<T, IsInput>::resolve_connection_values(const node_cfg& cfg)
{
    std::vector<T> result;

    for (auto iface : resolve_connections(cfg)) {
        if (iface->type() == type()) {
            result.emplace_back(get_value_from(iface));
        }
    }

    return result;
}

} // namespace miximus::nodes