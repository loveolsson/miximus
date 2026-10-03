#include "core/app_state.hpp"
#include "gpu/drawing.hpp"
#include "gpu/geometry.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "nodes/composite/source_labels.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

namespace {
using namespace miximus;
using namespace miximus::nodes;

gpu::texture_draw_s
label_geometry(const gpu::rect_s& cell, gpu::vec2i_t label_dimensions, gpu::vec2i_t target_dimensions, int inset)
{
    const int          left   = static_cast<int>(std::round(cell.pos.x * target_dimensions.x));
    const int          right  = static_cast<int>(std::round((cell.pos.x + cell.size.x) * target_dimensions.x));
    const int          bottom = static_cast<int>(std::round((cell.pos.y + cell.size.y) * target_dimensions.y));
    const gpu::vec2i_t position{
        left + ((right - left - label_dimensions.x) / 2),
        bottom - inset - label_dimensions.y,
    };
    const gpu::rect_s destination{
        .pos  = gpu::pixels_to_normalized(gpu::vec2_t(position), target_dimensions),
        .size = gpu::pixels_to_normalized(gpu::vec2_t(label_dimensions), target_dimensions),
    };
    return {.destination = destination};
}

class node_impl : public node_i
{
    input_interface_s<texture_source_info_s>      iface_tex_{*this, "tex"};
    input_interface_s<framebuffer_source_info_s>  iface_fb_in_{*this, "fb_in"};
    output_interface_s<framebuffer_source_info_s> iface_fb_out_{*this, "fb_out"};

    composite::source_labels_s labels_;

  public:
    explicit node_impl() { iface_tex_.set_max_connection_count(INT_MAX); }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const auto fb_source = iface_fb_in_.resolve_value(app, nodes, state);
        auto*      fb        = fb_source.texture;
        iface_fb_out_.set_value(fb_source);

        if (fb == nullptr) {
            labels_.clear();
            return;
        }

        auto textures = iface_tex_.resolve_values<9>(app, nodes, state);

        if (textures.empty()) {
            labels_.clear();
            return;
        }

        const size_t tex_count = textures.size();
        size_t       cols      = 1;
        for (; (cols * cols) < tex_count; cols++) {
        }

        const double box_dim = 1.0 / static_cast<double>(cols);

        const auto target_dimensions = fb->dimensions();
        const auto fill_mode         = state.get_enum_option_unchecked<gpu::fill_mode_e>("fill_mode");

        const bool   show_labels        = state.get_option<bool>("show_labels");
        const double label_scale        = static_cast<double>(target_dimensions.y) / 1080.0;
        const auto   scale_label_pixels = [label_scale](int pixels) {
            return std::max(1, static_cast<int>(std::round(pixels * label_scale)));
        };
        const int                  inset           = scale_label_pixels(12);
        const auto                 cell_dimensions = target_dimensions / static_cast<int>(cols);
        const render::pill_style_s label_style{
            .font_size  = scale_label_pixels(24),
            .padding_x  = scale_label_pixels(12),
            .padding_y  = scale_label_pixels(6),
            .max_width  = cell_dimensions.x - (2 * inset),
            .max_height = cell_dimensions.y - (2 * inset),
        };
        if (show_labels) {
            labels_.update(app, {textures.data(), textures.size()}, label_style);
        } else {
            labels_.clear();
        }

        for (size_t i = 0, y = 0; y < cols && i < tex_count; y++) {
            for (size_t x = 0; x < cols && i < tex_count; x++, i++) {
                const auto*       texture = textures[i].texture;
                const gpu::rect_s cell{
                    .pos  = {box_dim * static_cast<double>(x), box_dim * static_cast<double>(y)},
                    .size = {box_dim,                          box_dim                         },
                };
                if (texture != nullptr) {
                    const auto texture_draw =
                        gpu::calculate_texture_draw(cell, texture->dimensions(), target_dimensions, fill_mode);
                    gpu::draw_texture(app->commands(), texture, fb, {.geometry = texture_draw});
                }

                if (!show_labels) {
                    continue;
                }
                const auto* label = labels_.find(textures[i].name);
                if (label != nullptr) {
                    const auto geometry = label_geometry(cell, label->dimensions(), target_dimensions, inset);
                    gpu::draw_texture(app->commands(), label, fb, {.geometry = geometry});
                }
            }
        }
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",        "Infinite Multiviewer"                   },
            {"fill_mode",   enum_to_string(gpu::fill_mode_e::contain)},
            {"show_labels", false                                    },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "show_labels") {
            return normalize_option_value<bool>(value);
        }
        if (name == "fill_mode") {
            return normalize_enum_option_value<gpu::fill_mode_e>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "infinite_multiviewer"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_infinite_multiviewer_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
