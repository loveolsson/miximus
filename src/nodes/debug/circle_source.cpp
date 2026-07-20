#include "core/app_state.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <cmath>
#include <memory>
#include <numbers>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    output_interface_s<gpu::vec2_t> iface_res_{*this, "res"};

  public:
    explicit node_impl() = default;

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& state) final
    {
        const auto size   = state.get_option<gpu::vec2_t>("size");
        const auto center = state.get_option<gpu::vec2_t>("center");
        const auto speed  = state.get_option<double>("speed");
        const auto phase  = state.get_option<double>("phase");

        const double seconds       = utils::to_seconds(app->frame_info.pts);
        const double phase_radians = phase * std::numbers::pi_v<double> / 180.0;
        const double angle         = (seconds * speed) + phase_radians;
        const auto   result        = center + (gpu::vec2_t{std::cos(angle), std::sin(angle)} * size);

        iface_res_.set_value(result);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",   "Circle source"      },
            {"size",   gpu::vec2_t{1.0, 1.0}},
            {"center", gpu::vec2_t{0.0, 0.0}},
            {"speed",  0.1                  },
            {"phase",  0                    },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "speed" || name == "phase") {
            return normalize_option_value<double>(value);
        }
        if (name == "size" || name == "center") {
            return normalize_option_value<gpu::vec2_t>(value);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "circle_source"; }
};

} // namespace

namespace miximus::nodes::debug {

std::shared_ptr<node_i> create_circle_source_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::debug
