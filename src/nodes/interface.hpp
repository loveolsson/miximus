#pragma once
#include "core/app_state_fwd.hpp"
#include "gpu/types.hpp"
#include "nodes/node_map.hpp"

#include <climits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

class interface_i
{
  public:
    enum class type_e
    {
        invalid = -1,
        f64     = 0,
        vec2,
        rect,
        texture,
        framebuffer,
    };

    enum class dir_e
    {
        input,
        output,
    };

    interface_i()          = default;
    virtual ~interface_i() = default;

    bool               add_connection(con_set_t* connections, const connection_s& con, con_set_t& removed) const;
    const interface_i* resolve_connection(core::app_state_s*, const node_map_t&, const con_set_t&) const;
    void               set_max_connection_count(int count) { max_connection_count_ = count; }

    virtual dir_e  direction() const = 0;
    virtual type_e type() const      = 0;
    virtual bool   accepts(type_e /*type*/) const { return false; }

  protected:
    template <typename T>
    static type_e get_interface_type();

    int max_connection_count_{1};
};

template <typename T>
class input_interface_s : public interface_i
{
  public:
    input_interface_s()  = default;
    ~input_interface_s() = default;

    dir_e  direction() const final { return dir_e::input; }
    type_e type() const final { return get_interface_type<T>(); }
    bool   accepts(type_e type) const final;

    T resolve_value(core::app_state_s*, const node_map_t& nodes, const con_set_t& connections, T fallback = T{}) const;
};

template <typename T>
class output_interface_s : public interface_i
{
    T value_{};

  public:
    output_interface_s()
    {
        /**
         * Framebuffers are a special case only acceps a single output since the only way
         * to ensure all operations on a framebuffer has happens when the texture is read
         * is if the operations happen one after another with a single destination.
         * To send a framebuffer to multiple destinations if first need to go through a
         * framebuffer-to-texture adapter node.
         */
        if (type() != type_e::framebuffer) {
            set_max_connection_count(INT_MAX);
        }
    }
    ~output_interface_s() = default;

    dir_e  direction() const final { return dir_e::output; }
    type_e type() const final { return get_interface_type<T>(); }

    T    get_value() const { return value_; }
    void set_value(const T& value) { value_ = value; }
};

} // namespace miximus::nodes