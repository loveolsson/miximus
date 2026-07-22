#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "utils/observed_value.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::ndi;

auto log() { return getlog("ndi"); }

class node_impl : public node_i
{
    NDIlib_recv_instance_t      recv_{nullptr};
    NDIlib_framesync_instance_t framesync_{nullptr};

    std::mutex                                              upload_mutex_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    std::unique_ptr<gpu::framebuffer_s>                     framebuffer_; // rgb_f16 linearized output
    std::unique_ptr<gpu::textured_quad_s>                   textured_quad_;
    std::atomic<bool>                                       capture_running_;
    std::mutex                                              capture_mutex_;
    std::condition_variable                                 capture_cv_;
    bool                                                    capture_requested_{};
    std::thread                                             capture_thread_;

    utils::observed_value_s<uint64_t>    source_version_;
    utils::observed_value_s<std::string> connected_source_;

    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    void capture_loop(gpu::transfer::texture_upload_service_s* upload_service)
    {
        utils::observed_value_s<gpu::vec2i_t> current_dimensions;
        while (true) {
            {
                std::unique_lock lock(capture_mutex_);
                capture_cv_.wait(lock, [this] { return capture_requested_ || !capture_running_.load(); });
                if (!capture_running_.load()) {
                    break;
                }
                capture_requested_ = false;
            }

            NDIlib_video_frame_v2_t video_frame{};
            NDIlib_framesync_capture_video(framesync_, &video_frame, NDIlib_frame_format_type_progressive);
            if (video_frame.p_data == nullptr) {
                NDIlib_framesync_free_video(framesync_, &video_frame);
                continue;
            }

            const gpu::vec2i_t new_dim{video_frame.xres, video_frame.yres};
            const int          bytes_per_row = video_frame.xres * 4;
            const size_t       frame_size = static_cast<size_t>(bytes_per_row) * static_cast<size_t>(video_frame.yres);

            std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
            {
                const std::scoped_lock lock(upload_mutex_);
                if (current_dimensions.observe(new_dim) || !upload_stream_) {
                    const gpu::transfer::texture_transfer_requirements_s requirements{
                        .dimensions  = new_dim,
                        .format      = gpu::texture_s::format_e::bgra_u8,
                        .row_stride  = static_cast<size_t>(bytes_per_row),
                        .byte_size   = frame_size,
                        .host_access = gpu::transfer::host_access_e::overwrite,
                    };
                    upload_stream_ = upload_service->create_stream({
                        .requirements      = requirements,
                        .max_slots         = 4,
                        .generate_mip_maps = false,
                    });
                }
                stream = upload_stream_;
            }

            auto upload = stream->try_acquire();
            if (upload) {
                const auto destination = upload->bytes();
                auto*      src         = video_frame.p_data;
                if (destination.size() < frame_size) {
                    NDIlib_framesync_free_video(framesync_, &video_frame);
                    continue;
                }
                if (video_frame.line_stride_in_bytes == bytes_per_row) {
                    std::memcpy(destination.data(), src, frame_size);
                } else {
                    for (int y = 0; y < video_frame.yres; ++y) {
                        const auto offset = static_cast<size_t>(y) * static_cast<size_t>(bytes_per_row);
                        std::memcpy(destination.data() + offset, src, static_cast<size_t>(bytes_per_row));
                        src += video_frame.line_stride_in_bytes;
                    }
                }
                upload->submit();
            }

            NDIlib_framesync_free_video(framesync_, &video_frame);
        }
    }

    void free_receiver()
    {
        capture_running_ = false;
        capture_cv_.notify_one();
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        capture_requested_ = false;
        if (framesync_ != nullptr) {
            NDIlib_framesync_destroy(framesync_);
            framesync_ = nullptr;
        }
        if (recv_ != nullptr) {
            NDIlib_recv_destroy(recv_);
            recv_ = nullptr;
        }
        {
            const std::scoped_lock lock(upload_mutex_);
            upload_stream_.reset();
        }
        framebuffer_.reset();
        textured_quad_.reset();
        connected_source_.reset();
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override { free_receiver(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        auto* sr = app->status_registry();

        const auto current_version = app->ndi_registry()->get_source_list_version();
        if (source_version_.observe(current_version)) {
            sr->write(id_, "source_names", app->ndi_registry()->get_source_options());
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

        if (connected_source_.would_change(source_name)) {
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

            connected_source_.commit(source_name);
            capture_running_ = true;
            capture_thread_  = std::thread(&node_impl::capture_loop, this, app->texture_upload_service());
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (framesync_ == nullptr) {
            iface_tex_.set_value(nullptr);
            return;
        }

        {
            const std::scoped_lock lock(capture_mutex_);
            capture_requested_ = true;
        }
        capture_cv_.notify_one();

        std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
        {
            const std::scoped_lock lock(upload_mutex_);
            stream = upload_stream_;
        }
        if (!stream) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        // Poll only; the render thread never waits for an upload. Until the new
        // frame is ready, keep publishing the previous converted frame.
        auto upload_texture = stream->consume_latest();
        if (upload_texture == nullptr) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        if (!framebuffer_ || framebuffer_->texture()->display_dimensions() != upload_texture->display_dimensions()) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(upload_texture->display_dimensions(),
                                                                gpu::texture_s::format_e::rgb_f16);
        }

        // Linearize from Rec.709 gamma into rgb_f16 framebuffer
        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::strip_gamma);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_->draw(upload_texture);
        gpu::framebuffer_s::end_render();

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

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "source_name") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "enabled") {
            return normalize_option_value<bool>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "ndi_input"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
