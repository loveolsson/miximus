#include "gpu/framebuffer.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "utils.hpp"

#include <glm/glm.hpp>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<gpu::vec2i_t>         iface_size_;
    output_interface_s<gpu::framebuffer_s*> iface_fb_;

    std::unique_ptr<gpu::framebuffer_s> framebuffer_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("size", &iface_size_);
        interfaces_.emplace("fb", &iface_fb_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto size_opt = state.get_option<gpu::vec2i_t>("size");
        auto size     = iface_size_.resolve_value(app, nodes, state.get_connection_set("size"), size_opt);

        size = glm::max(size, gpu::vec2i_t{128, 128});

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != size) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(size, gpu::texture_s::colorspace_e::RGBA);
        }

        framebuffer_->bind();
        auto dim = framebuffer_->texture()->texture_dimensions();
        glViewport(0, 0, dim.x, dim.y);
        glClearColor(0, 0, 0, 0);
        glClear(GLbitfield(GL_COLOR_BUFFER_BIT) | GLbitfield(GL_DEPTH_BUFFER_BIT));
        gpu::framebuffer_s::unbind();

        iface_fb_.set_value(framebuffer_.get());
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Framebuffer"},
            {"size", nlohmann::json::array({1920, 1080})},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "size") {
            return detail::validate_option<gpu::vec2i_t>(value);
        }

        return false;
    }

    std::string_view type() const final { return "framebuffer"; }
};

} // namespace

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_framebuffer_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::utils
