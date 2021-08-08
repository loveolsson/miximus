#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "nodes/interface.hpp"
#include "utils.hpp"

#include <glm/glm.hpp>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<gpu::framebuffer_s*> iface_fb_;
    output_interface_s<gpu::texture_s*>    iface_tex_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("fb", &iface_fb_);
        interfaces_.emplace("tex", &iface_tex_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        gpu::texture_s* texture = nullptr;

        auto* fb = iface_fb_.resolve_value(app, nodes, state.get_connection_set("fb"));
        if (fb != nullptr) {
            texture = fb->texture();
        }

        iface_tex_.set_value(texture);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Framebuffer to texture"},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final { return false; }

    std::string_view type() const final { return "framebuffer_to_texture"; }
};

} // namespace

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_framebuffer_to_texture_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::utils
