#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <memory>
#include <string_view>

namespace {
using namespace miximus;
using namespace miximus::nodes;

template <typename T>
class node_impl : public node_i
{
    input_interface_s<T>  input_;
    output_interface_s<T> output_;
    std::string_view      type_;
    std::string_view      name_;

  public:
    node_impl(std::string_view type, std::string_view name, std::string_view input_name, std::string_view output_name)
        : input_(*this, input_name)
        , output_(*this, output_name)
        , type_(type)
        , name_(name)
    {
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto* source = input_.resolve_value(app, nodes, state).texture;
        if (source != nullptr) {
            output_.set_value({.texture = source, .name = state.get_option_string_view("source_name")});
        } else {
            output_.set_value({});
        }
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",        name_},
            {"source_name", ""   }
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "source_name") {
            return normalize_option_value<std::string_view>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return type_; }
};

} // namespace

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_set_texture_name_node()
{
    return std::make_shared<node_impl<texture_source_info_s>>(
        "set_texture_name", "Set Texture Name", "tex_in", "tex_out");
}

std::shared_ptr<node_i> create_set_framebuffer_name_node()
{
    return std::make_shared<node_impl<framebuffer_source_info_s>>(
        "set_framebuffer_name", "Set Framebuffer Name", "fb_in", "fb_out");
}

} // namespace miximus::nodes::utils
