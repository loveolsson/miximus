#pragma once
#include "core/app_state_fwd.hpp"
#include "gpu/types.hpp"
#include "nodes/interface_type.hpp"
#include "nodes/node_map.hpp"

#include <boost/container/small_vector.hpp>

#include <climits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace miximus::nodes {

class interface_i
{
    using resolved_cons_t = boost::container::small_vector<const interface_i*, 4>;

  public:
    enum class dir_e
    {
        input,
        output,
    };

    interface_i(std::string_view name)
        : name_(name)
    {
    }
    virtual ~interface_i() = default;

    bool add_connection(con_set_t* connections, const connection_s& con, con_set_t* removed) const;
    void set_max_connection_count(int count) { max_connection_count_ = count; }

    virtual dir_e            direction() const = 0;
    virtual interface_type_e type() const      = 0;
    virtual bool             accepts(interface_type_e /*type*/) const { return false; }
    std::string_view         name() const { return name_; }

  protected:
    resolved_cons_t resolve_connections(core::app_state_s*, const node_map_t&, const node_state_s&) const;

    int              max_connection_count_{1};
    std::string_view name_;
};

template <typename T>
class input_interface_s : public interface_i
{
    template <size_t S>
    using resolved_values_t = boost::container::small_vector<T, S>;

  public:
    input_interface_s(std::string_view name)
        : interface_i(name)
    {
    }
    ~input_interface_s() = default;

    dir_e            direction() const final { return dir_e::input; }
    interface_type_e type() const final { return get_interface_type<T>(); }
    bool             accepts(interface_type_e type) const final;

    static T cast_iface_to_value(const interface_i* iface, T const& fallback);

    T resolve_value(core::app_state_s*  app,
                    const node_map_t&   nodes,
                    const node_state_s& state,
                    T const&            fallback = T{}) const
    {
        auto ifaces = resolve_connections(app, nodes, state);

        assert(ifaces.size() <= 1);
        assert(max_connection_count_ == 1); // Should only be called on interfaces expecting a single value

        if (!ifaces.empty() && ifaces.front() != nullptr) {
            return cast_iface_to_value(ifaces.front(), fallback);
        }

        return fallback;
    }

    template <size_t S = 4>
    resolved_values_t<S> resolve_values(core::app_state_s*  app,
                                        const node_map_t&   nodes,
                                        const node_state_s& state,
                                        T const&            fallback = T{}) const
    {
        resolved_values_t<S> res;

        auto ifaces = resolve_connections(app, nodes, state);

        res.reserve(ifaces.size());
        assert(max_connection_count_ > 1); // Should only be called on interfaces expecting multiple values

        for (const auto* iface : ifaces) {
            if (iface == nullptr) {
                res.emplace_back(fallback);
            } else {
                res.emplace_back(cast_iface_to_value(iface, fallback));
            }
        }

        return res;
    }
};

template <typename T>
class output_interface_s : public interface_i
{
    T value_{};

  public:
    output_interface_s(std::string_view name)
        : interface_i(name)
    {
        /**
         * Framebuffers are a special case only acceps a single output since the only way
         * to ensure all operations on a framebuffer has happens when the texture is read
         * is if the operations happen one after another with a single destination.
         * To send a framebuffer to multiple destinations if first need to go through a
         * framebuffer-to-texture adapter node.
         */
        if (type() != interface_type_e::framebuffer) {
            set_max_connection_count(INT_MAX);
        }
    }
    ~output_interface_s() = default;

    dir_e            direction() const final { return dir_e::output; }
    interface_type_e type() const final { return get_interface_type<T>(); }

    T    get_value() const { return value_; }
    void set_value(const T& value) { value_ = value; }
};

} // namespace miximus::nodes
