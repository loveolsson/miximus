#include "gpu/framebuffer.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <queue>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    output_interface_s<gpu::texture_s*> iface_tex_;

    std::unique_ptr<gpu::framebuffer_s> framebuffer_;

  public:
    explicit node_impl() { interfaces_.emplace("tex", &iface_tex_); }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "FFmpeg player"},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "file_path") {
            return validate_option<std::string_view>(value);
        }

        return false;
    }

    std::string_view type() const final { return "ffmpeg_player"; }
};

} // namespace

namespace miximus::nodes::ffmpeg {

std::shared_ptr<node_i> create_ffmpeg_player_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::ffmpeg
