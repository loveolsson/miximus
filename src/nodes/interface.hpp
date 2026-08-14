#pragma once
#include "core/app_state_fwd.hpp"
#include "gpu/types.hpp"
#include "nodes/disconnected_value_provider.hpp"
#include "nodes/interface_type.hpp"
#include "nodes/node_fwd.hpp"
#include "nodes/node_map_fwd.hpp"
#include "utils/is_finite.hpp"

#include <boost/container/small_vector.hpp>

#include <cassert>
#include <cstddef>
#include <limits>
#include <span>
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

    interface_i(node_i& owner, std::string_view name, dir_e direction, interface_type_e type);
    virtual ~interface_i() = default;

    interface_i(const interface_i&)            = delete;
    interface_i(interface_i&&)                 = delete;
    interface_i& operator=(const interface_i&) = delete;
    interface_i& operator=(interface_i&&)      = delete;

    void add_connection(con_set_t* connections, const connection_s& con, con_set_t* removed) const;
    void set_max_connection_count(size_t count) noexcept { max_connection_count_ = count; }

    std::span<const connection_s> connections(const node_state_s& state) const;
    static void submit_dependencies(core::app_state_s*, const node_map_t&, std::span<const connection_s>);

    dir_e            direction() const noexcept { return direction_; }
    interface_type_e type() const noexcept { return type_; }
    virtual bool     accepts(interface_type_e /*type*/) const noexcept { return false; }
    std::string_view name() const noexcept { return name_; }

  protected:
    static const interface_i* resolve_connection(core::app_state_s*, const node_map_t&, const connection_s&);
    static resolved_cons_t    resolve_connections(core::app_state_s*, const node_map_t&, std::span<const connection_s>);

    size_t           max_connection_count_{1};
    std::string_view name_;

  private:
    const dir_e            direction_;
    const interface_type_e type_;
};

template <typename T>
class input_interface_s : public interface_i
{
    template <size_t S>
    using resolved_values_t = boost::container::small_vector<T, S>;

    [[no_unique_address]] mutable detail::disconnected_value_provider_s<T> disconnected_value_provider_;

  public:
    input_interface_s(node_i& owner, std::string_view name)
        : interface_i(owner, name, dir_e::input, get_interface_type<T>())
    {
    }
    ~input_interface_s() = default;

    bool accepts(interface_type_e type) const noexcept final
    {
        constexpr auto target_type = get_interface_type<T>();
        switch (target_type) {
            case interface_type_e::vec2:
                return type == interface_type_e::vec2 || type == interface_type_e::f64;
            case interface_type_e::texture:
                return type == interface_type_e::texture || type == interface_type_e::framebuffer;
            default:
                return type == target_type;
        }
    }
    static T cast_iface_to_value(const interface_i* iface, const T& fallback);

    T resolve_value(core::app_state_s*  app,
                    const node_map_t&   nodes,
                    const node_state_s& state,
                    T const&            fallback = T{}) const
    {
        assert(max_connection_count_ == 1); // Should only be called on interfaces expecting a single value

        const auto connected = connections(state);
        assert(connected.size() <= 1);

        if (!connected.empty()) {
            disconnected_value_provider_.release();
            const auto* iface = resolve_connection(app, nodes, connected.front());
            return iface != nullptr ? cast_iface_to_value(iface, fallback) : fallback;
        }

        return disconnected_value_provider_.get(app, fallback);
    }

    template <size_t S = 4>
    resolved_values_t<S> resolve_values(core::app_state_s*  app,
                                        const node_map_t&   nodes,
                                        const node_state_s& state,
                                        T const&            fallback = T{}) const
    {
        resolved_values_t<S> res;

        auto ifaces = resolve_connections(app, nodes, connections(state));

        res.reserve(ifaces.size());
        assert(max_connection_count_ > 1); // Should only be called on interfaces expecting multiple values

        for (const auto iface : ifaces) {
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
    output_interface_s(node_i& owner, std::string_view name)
        : interface_i(owner, name, dir_e::output, get_interface_type<T>())
    {
        /**
         * Framebuffers are a special case only acceps a single output since the only way
         * to ensure all operations on a framebuffer has happens when the texture is read
         * is if the operations happen one after another with a single destination.
         * To send a framebuffer to multiple destinations if first need to go through a
         * framebuffer-to-texture adapter node.
         */
        if (type() != interface_type_e::framebuffer) {
            set_max_connection_count(std::numeric_limits<size_t>::max());
        }
    }
    ~output_interface_s() = default;

    T    get_value() const { return value_; }
    void set_value(const T& value) { value_ = utils::is_finite(value) ? value : T{}; }
};

template <>
double input_interface_s<double>::cast_iface_to_value(const interface_i* iface, const double& fallback);
template <>
gpu::vec2_t input_interface_s<gpu::vec2_t>::cast_iface_to_value(const interface_i* iface, const gpu::vec2_t& fallback);
template <>
gpu::rect_s input_interface_s<gpu::rect_s>::cast_iface_to_value(const interface_i* iface, const gpu::rect_s& fallback);
template <>
gpu::texture_s* input_interface_s<gpu::texture_s*>::cast_iface_to_value(const interface_i*     iface,
                                                                        gpu::texture_s* const& fallback);
template <>
gpu::framebuffer_s* input_interface_s<gpu::framebuffer_s*>::cast_iface_to_value(const interface_i*         iface,
                                                                                gpu::framebuffer_s* const& fallback);

} // namespace miximus::nodes
