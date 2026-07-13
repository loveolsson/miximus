#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/transfer.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"
#include "registry.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::ndi;

auto log() { return getlog("ndi"); }

class node_impl : public node_i
{
    NDIlib_recv_instance_t      recv_{nullptr};
    NDIlib_framesync_instance_t framesync_{nullptr};

    std::unique_ptr<gpu::texture_s>            upload_texture_; // bgra_u8 upload target
    std::unique_ptr<gpu::framebuffer_s>        framebuffer_;    // rgb_f16 linearized output
    std::unique_ptr<gpu::draw_state_s>         draw_state_;
    std::unique_ptr<gpu::transfer::transfer_i> transfer_;
    gpu::vec2i_t                               current_dim_{};

    uint64_t    last_source_version_{std::numeric_limits<uint64_t>::max()};
    std::string connected_source_;

    output_interface_s<gpu::texture_s*> iface_tex_{"tex"};

    void free_receiver()
    {
        if (framesync_ != nullptr) {
            NDIlib_framesync_destroy(framesync_);
            framesync_ = nullptr;
        }
        if (recv_ != nullptr) {
            NDIlib_recv_destroy(recv_);
            recv_ = nullptr;
        }
        if (transfer_ && upload_texture_) {
            gpu::transfer::transfer_i::unregister_texture(transfer_->type(), upload_texture_.get());
        }
        transfer_.reset();
        upload_texture_.reset();
        framebuffer_.reset();
        draw_state_.reset();
        current_dim_      = {};
        connected_source_ = {};
    }

  public:
    explicit node_impl() { register_interface(&iface_tex_); }

    ~node_impl() override { free_receiver(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* /*traits*/) final
    {
        auto* sr = app->status_registry();

        const auto current_version = app->ndi_registry()->get_source_list_version();
        if (current_version != last_source_version_) {
            last_source_version_ = current_version;
            sr->write(id_, "source_names", nlohmann::json(app->ndi_registry()->get_source_names()));
        }

        sr->write(id_, "connected", framesync_ != nullptr);

        const auto source_name = state.get_option<std::string>("source_name");
        const auto enabled     = state.get_option<bool>("enabled");

        if (!enabled || source_name.empty()) {
            if (recv_ != nullptr) {
                free_receiver();
            }
            return;
        }

        if (source_name != connected_source_) {
            free_receiver();

            log()->info("Connecting NDI input to \"{}\"", source_name);

            NDIlib_recv_create_v3_t create{};
            create.color_format       = NDIlib_recv_color_format_BGRX_BGRA;
            create.bandwidth          = NDIlib_recv_bandwidth_highest;
            create.allow_video_fields = false;
            create.p_ndi_recv_name    = id_.c_str();

            NDIlib_source_t src{};
            src.p_ndi_name              = source_name.c_str();
            create.source_to_connect_to = src;

            recv_ = NDIlib_recv_create_v3(&create);
            if (recv_ == nullptr) {
                log()->error("NDIlib_recv_create_v3 failed for \"{}\"", source_name);
                return;
            }

            framesync_ = NDIlib_framesync_create(recv_);
            if (framesync_ == nullptr) {
                log()->error("NDIlib_framesync_create failed");
                NDIlib_recv_destroy(recv_);
                recv_ = nullptr;
                return;
            }

            connected_source_ = source_name;
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (framesync_ == nullptr) {
            iface_tex_.set_value(nullptr);
            return;
        }

        NDIlib_video_frame_v2_t video_frame{};
        NDIlib_framesync_capture_video(framesync_, &video_frame, NDIlib_frame_format_type_progressive);

        if (video_frame.p_data == nullptr) {
            NDIlib_framesync_free_video(framesync_, &video_frame);
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        const gpu::vec2i_t new_dim{video_frame.xres, video_frame.yres};
        const int          bytes_per_row = video_frame.xres * 4;
        const size_t       frame_size    = static_cast<size_t>(bytes_per_row) * video_frame.yres;

        if (new_dim != current_dim_ || !transfer_) {
            if (transfer_ && upload_texture_) {
                gpu::transfer::transfer_i::unregister_texture(transfer_->type(), upload_texture_.get());
            }
            current_dim_    = new_dim;
            upload_texture_ = std::make_unique<gpu::texture_s>(new_dim, gpu::texture_s::format_e::bgra_u8);
            framebuffer_    = std::make_unique<gpu::framebuffer_s>(new_dim, gpu::texture_s::format_e::rgb_f16);
            transfer_       = gpu::transfer::transfer_i::create_transfer(gpu::transfer::transfer_i::get_prefered_type(),
                                                                   frame_size,
                                                                   gpu::transfer::transfer_i::direction_e::cpu_to_gpu);
            gpu::transfer::transfer_i::register_texture(transfer_->type(), upload_texture_.get());
        }

        // Copy NDI frame into transfer buffer, handling non-tight stride
        auto* dst = static_cast<uint8_t*>(transfer_->ptr());
        auto* src = video_frame.p_data;
        if (video_frame.line_stride_in_bytes == bytes_per_row) {
            std::memcpy(dst, src, frame_size);
        } else {
            for (int y = 0; y < video_frame.yres; ++y) {
                std::memcpy(dst, src, bytes_per_row);
                dst += bytes_per_row;
                src += video_frame.line_stride_in_bytes;
            }
        }

        NDIlib_framesync_free_video(framesync_, &video_frame);

        // Upload to GPU via transfer abstraction
        transfer_->perform_copy();
        transfer_->wait_for_copy();
        transfer_->perform_transfer(upload_texture_.get());

        // Linearize from Rec.709 gamma into rgb_f16 framebuffer
        if (!draw_state_) {
            draw_state_ = std::make_unique<gpu::draw_state_s>();
            auto shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::strip_gamma);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        framebuffer_->begin_render();
        upload_texture_->bind(0);
        auto shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", gpu::vec2_t{0.0, 0.0});
        shader->set_uniform("scale", gpu::vec2_t{1.0, 1.0});
        draw_state_->draw();
        gpu::texture_s::unbind(0);
        gpu::framebuffer_s::end_render();

        // Signal that GL is done sampling upload_texture; DVP may write again next frame.
        gpu::transfer::transfer_i::end_texture_use(transfer_->type(), upload_texture_.get());

        auto* out_tex = framebuffer_->texture();
        out_tex->generate_mip_maps();
        iface_tex_.set_value(out_tex);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",        "NDI Input"},
            {"enabled",     true       },
            {"source_name", ""         },
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "source_name") {
            return validate_option<std::string_view>(value);
        }
        if (name == "enabled") {
            return validate_option<bool>(value);
        }
        return false;
    }

    std::string_view type() const final { return "ndi_input"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
