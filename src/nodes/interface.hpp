#pragma once
#include "gpu/types.hpp"
#include "types/connection_set.hpp"
#include "types/interface_type.hpp"
#include "types/node_map.hpp"

#include <climits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

class interface_i
{
  public:
    enum class dir
    {
        input,
        output,
    };

    interface_i()          = default;
    virtual ~interface_i() = default;

    bool         add_connection(con_set_t* connections, const connection& con, con_set_t& removed) const;
    interface_i* resolve_connection(node_map_t&, const con_set_t&) const;

    virtual dir              direction() const = 0;
    virtual interface_type_e type() const      = 0;
    virtual bool             accepts(interface_type_e /*type*/) const { return false; }
};

template <typename T>
class input_interface : public interface_i
{
  public:
    input_interface()  = default;
    ~input_interface() = default;

    dir              direction() const final { return dir::input; }
    interface_type_e type() const final { return get_interface_type<T>(); }
    bool             accepts(interface_type_e type) const final;

    T resolve_value(node_map_t& nodes, const con_set_t& connections) const;
};

template <typename T>
class output_interface : public interface_i
{
    T value_{};

  public:
    output_interface()  = default;
    ~output_interface() = default;

    dir              direction() const final { return dir::output; }
    interface_type_e type() const final { return get_interface_type<T>(); }

    T    get_value() const { return value_; }
    void set_value(const T& value) { value_ = value; }
};

} // namespace miximus::nodes