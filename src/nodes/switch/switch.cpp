#include "gpu/geometry.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;

constexpr std::array INPUT_NAMES{"a", "b", "c", "d", "e", "f", "g", "h"};

template <typename T, size_t SlotCount>
class node_impl : public node_i
{
    static_assert(SlotCount <= INPUT_NAMES.size());

    template <size_t... Indices>
    static auto make_inputs(node_i& owner, std::index_sequence<Indices...> /*indices*/)
    {
        return std::array<input_interface_s<T>, SlotCount>{
            input_interface_s<T>{owner, INPUT_NAMES.at(Indices)}
            ...
        };
    }

    input_interface_s<double>                   active_{*this, "active"};
    std::array<input_interface_s<T>, SlotCount> inputs_{make_inputs(*this, std::make_index_sequence<SlotCount>{})};
    output_interface_s<T>                       output_;
    std::string_view                            type_;
    std::string_view                            name_;

    static size_t active_index(double active)
    {
        const double clamped = std::clamp(std::floor(active), 1.0, static_cast<double>(SlotCount));
        return static_cast<size_t>(clamped - 1.0);
    }

  public:
    node_impl(std::string_view type, std::string_view name, std::string_view output_name)
        : output_(*this, output_name)
        , type_(type)
        , name_(name)
    {
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const int    active_option = state.get_option<int>("active", 1);
        const double active_value  = active_.resolve_value(app, nodes, state, static_cast<double>(active_option));
        const auto   index         = active_index(active_value);
        const auto&  input         = inputs_.at(index);
        output_.set_value(input.resolve_value(app, nodes, state));
    }

    void submit(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const auto active_connections = active_.connections(state);
        interface_i::submit_dependencies(app, nodes, active_connections);
        if (!active_connections.empty()) {
            for (const auto& input : inputs_) {
                interface_i::submit_dependencies(app, nodes, input.connections(state));
            }
            return;
        }

        const auto  index = active_index(static_cast<double>(state.get_option<int>("active", 1)));
        const auto& input = inputs_.at(index);
        interface_i::submit_dependencies(app, nodes, input.connections(state));
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",   name_},
            {"active", 1    },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "active") {
            return normalize_option_value<int>(value, 1, static_cast<int>(SlotCount));
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return type_; }
};

} // namespace

namespace miximus::nodes::switch_nodes {

std::shared_ptr<node_i> create_switch_f64_4_node()
{
    return std::make_shared<node_impl<double, 4>>("switch_f64_4", "Switch Number x4", "res");
}

std::shared_ptr<node_i> create_switch_f64_8_node()
{
    return std::make_shared<node_impl<double, 8>>("switch_f64_8", "Switch Number x8", "res");
}

std::shared_ptr<node_i> create_switch_vec2_4_node()
{
    return std::make_shared<node_impl<gpu::vec2_t, 4>>("switch_vec2_4", "Switch Vec2 x4", "res");
}

std::shared_ptr<node_i> create_switch_vec2_8_node()
{
    return std::make_shared<node_impl<gpu::vec2_t, 8>>("switch_vec2_8", "Switch Vec2 x8", "res");
}

std::shared_ptr<node_i> create_switch_rect_4_node()
{
    return std::make_shared<node_impl<gpu::rect_s, 4>>("switch_rect_4", "Switch Rect x4", "res");
}

std::shared_ptr<node_i> create_switch_rect_8_node()
{
    return std::make_shared<node_impl<gpu::rect_s, 8>>("switch_rect_8", "Switch Rect x8", "res");
}

std::shared_ptr<node_i> create_switch_tex_4_node()
{
    return std::make_shared<node_impl<gpu::texture_s*, 4>>("switch_tex_4", "Switch Texture x4", "tex");
}

std::shared_ptr<node_i> create_switch_tex_8_node()
{
    return std::make_shared<node_impl<gpu::texture_s*, 8>>("switch_tex_8", "Switch Texture x8", "tex");
}

} // namespace miximus::nodes::switch_nodes
