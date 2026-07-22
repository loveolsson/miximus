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
#include <optional>
#include <string_view>

namespace {
using namespace miximus;
using namespace miximus::nodes;

constexpr std::array<std::string_view, 8> INPUT_NAMES{"a", "b", "c", "d", "e", "f", "g", "h"};

template <typename T>
struct switch_traits_s;

template <>
struct switch_traits_s<double>
{
    static constexpr std::string_view type_4{"switch_f64_4"};
    static constexpr std::string_view type_8{"switch_f64_8"};
    static constexpr std::string_view name_4{"Switch Number x4"};
    static constexpr std::string_view name_8{"Switch Number x8"};
    static constexpr std::string_view output{"res"};
};

template <>
struct switch_traits_s<gpu::vec2_t>
{
    static constexpr std::string_view type_4{"switch_vec2_4"};
    static constexpr std::string_view type_8{"switch_vec2_8"};
    static constexpr std::string_view name_4{"Switch Vec2 x4"};
    static constexpr std::string_view name_8{"Switch Vec2 x8"};
    static constexpr std::string_view output{"res"};
};

template <>
struct switch_traits_s<gpu::rect_s>
{
    static constexpr std::string_view type_4{"switch_rect_4"};
    static constexpr std::string_view type_8{"switch_rect_8"};
    static constexpr std::string_view name_4{"Switch Rect x4"};
    static constexpr std::string_view name_8{"Switch Rect x8"};
    static constexpr std::string_view output{"res"};
};

template <>
struct switch_traits_s<gpu::texture_s*>
{
    static constexpr std::string_view type_4{"switch_tex_4"};
    static constexpr std::string_view type_8{"switch_tex_8"};
    static constexpr std::string_view name_4{"Switch Texture x4"};
    static constexpr std::string_view name_8{"Switch Texture x8"};
    static constexpr std::string_view output{"tex"};
};

template <typename T, size_t SlotCount>
class switch_node_s : public node_i
{
    static_assert(SlotCount == 4 || SlotCount == 8);

    input_interface_s<double>                                  active_{*this, "active"};
    std::array<std::optional<input_interface_s<T>>, SlotCount> inputs_;
    output_interface_s<T>                                      output_{*this, switch_traits_s<T>::output};

    static constexpr std::string_view node_type()
    {
        if constexpr (SlotCount == 4) {
            return switch_traits_s<T>::type_4;
        } else {
            return switch_traits_s<T>::type_8;
        }
    }

    static constexpr std::string_view default_name()
    {
        if constexpr (SlotCount == 4) {
            return switch_traits_s<T>::name_4;
        } else {
            return switch_traits_s<T>::name_8;
        }
    }

  public:
    switch_node_s()
    {
        for (size_t index = 0; index < inputs_.size(); ++index) {
            inputs_.at(index).emplace(*this, INPUT_NAMES.at(index));
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const int    active_option = state.get_option<int>("active", 1);
        const double active_value  = active_.resolve_value(app, nodes, state, static_cast<double>(active_option));
        const double finite_value  = std::isfinite(active_value) ? active_value : static_cast<double>(active_option);
        const double clamped_value = std::clamp(std::floor(finite_value), 1.0, static_cast<double>(SlotCount));
        const int    active        = static_cast<int>(clamped_value);
        const auto   index         = static_cast<size_t>(active - 1);
        const auto&  input         = inputs_.at(index);
        if (!input.has_value()) {
            output_.set_value({});
            return;
        }

        output_.set_value(input->resolve_value(app, nodes, state));
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",   default_name()},
            {"active", 1             },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "active") {
            return normalize_option_value<int>(value, 1, static_cast<int>(SlotCount));
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return node_type(); }
};

template <typename T, size_t SlotCount>
std::shared_ptr<node_i> create_switch_node()
{
    return std::make_shared<switch_node_s<T, SlotCount>>();
}

} // namespace

namespace miximus::nodes::switch_nodes {

std::shared_ptr<node_i> create_switch_f64_4_node() { return create_switch_node<double, 4>(); }
std::shared_ptr<node_i> create_switch_f64_8_node() { return create_switch_node<double, 8>(); }
std::shared_ptr<node_i> create_switch_vec2_4_node() { return create_switch_node<gpu::vec2_t, 4>(); }
std::shared_ptr<node_i> create_switch_vec2_8_node() { return create_switch_node<gpu::vec2_t, 8>(); }
std::shared_ptr<node_i> create_switch_rect_4_node() { return create_switch_node<gpu::rect_s, 4>(); }
std::shared_ptr<node_i> create_switch_rect_8_node() { return create_switch_node<gpu::rect_s, 8>(); }
std::shared_ptr<node_i> create_switch_tex_4_node() { return create_switch_node<gpu::texture_s*, 4>(); }
std::shared_ptr<node_i> create_switch_tex_8_node() { return create_switch_node<gpu::texture_s*, 8>(); }

} // namespace miximus::nodes::switch_nodes
