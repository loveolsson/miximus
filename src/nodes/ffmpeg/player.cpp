#include "gpu/framebuffer.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <memory>
#include <queue>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    std::unique_ptr<gpu::framebuffer_s> framebuffer_;

  public:
    explicit node_impl() = default;

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, prepare_result_s* /*result*/) final {}

    void execute(core::app_state_s* /*app*/, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "FFmpeg player"},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "file_path") {
            return normalize_option_value<std::string_view>(value);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "ffmpeg_player"; }
};

} // namespace

namespace miximus::nodes::ffmpeg {

std::shared_ptr<node_i> create_ffmpeg_player_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::ffmpeg
