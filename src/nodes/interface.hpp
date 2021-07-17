#pragma once
#include "core/app_state.hpp"
#include "gpu/types.hpp"
#include "nodes/node_map.hpp"

#include <climits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

enum class interface_type_e
{
    invalid = -1,
    f64     = 0,
    i64,
    vec2,
    rect,
    texture,
    framebuffer,
};

class interface_i
{
  protected:
    template <typename T>
    static interface_type_e get_interface_type();

  public:
    enum class dir_e
    {
        input,
        output,
    };

    interface_i()          = default;
    virtual ~interface_i() = default;

    bool               add_connection(con_set_t* connections, const connection_s& con, con_set_t& removed) const;
    const interface_i* resolve_connection(core::app_state_s&, const node_map_t&, const con_set_t&) const;

    virtual dir_e            direction() const = 0;
    virtual interface_type_e type() const      = 0;
    virtual bool             accepts(interface_type_e /*type*/) const { return false; }
};

template <typename T>
class input_interface_s : public interface_i
{
  public:
    input_interface_s()  = default;
    ~input_interface_s() = default;

    dir_e            direction() const final { return dir_e::input; }
    interface_type_e type() const final { return get_interface_type<T>(); }
    bool             accepts(interface_type_e type) const final;

    T resolve_value(core::app_state_s&,
                    const node_map_t& nodes,
                    const con_set_t&  connections,
                    const T&          fallback = T()) const;
};

template <typename T>
class output_interface_s : public interface_i
{
    T value_{};

  public:
    output_interface_s()  = default;
    ~output_interface_s() = default;

    dir_e            direction() const final { return dir_e::output; }
    interface_type_e type() const final { return get_interface_type<T>(); }

    T    get_value() const { return value_; }
    void set_value(const T& value) { value_ = value; }
};

} // namespace miximus::nodes