#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/transfer/texture_download.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "utils/flicks.hpp"
#include "utils/observed_value.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;

auto log() { return getlog("ndi"); }

class node_impl : public node_i
{
    struct frame_rate_s
    {
        int numerator{};
        int denominator{};
    };

    NDIlib_send_instance_t                 sender_{nullptr};
    utils::observed_value_s<std::string>   sender_name_;
    utils::observed_value_s<utils::flicks> frame_duration_;
    std::atomic<frame_rate_s>              frame_rate_{frame_rate_s{}};

    std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_;
    utils::observed_value_s<gpu::vec2i_t>                     stream_dimensions_;

    std::mutex              cv_mutex_;
    std::condition_variable cv_;
    std::atomic<bool>       worker_running_{false};
    std::thread             worker_thread_;

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    static constexpr auto COLOR_METADATA = R"(<ndi_color_info primaries="bt_709" transfer="bt_709" matrix="bt_709"/>)";

    // Worker thread: no GL context needed. Download frames are published only
    // after their transfer has completed and remain pinned through async send.
    void worker_loop()
    {
        std::optional<gpu::transfer::texture_download_frame_s> inflight;

        while (worker_running_.load()) {
            std::shared_ptr<gpu::transfer::texture_download_stream_s> stream;
            {
                std::unique_lock lock(cv_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(2));
                if (!worker_running_.load()) {
                    break;
                }
                stream = download_stream_;
            }
            if (!stream) {
                continue;
            }
            auto frame = stream->try_consume_latest();
            if (!frame) {
                continue;
            }

            const utils::flicks pts(static_cast<utils::flicks::rep>(frame->tag()));
            const auto          frame_rate  = frame_rate_.load(std::memory_order_relaxed);
            const auto          frame_bytes = frame->bytes();

            NDIlib_video_frame_v2_t ndi_frame{};
            const auto              dim    = stream->desc().requirements.dimensions;
            ndi_frame.xres                 = dim.x;
            ndi_frame.yres                 = dim.y;
            ndi_frame.FourCC               = NDIlib_FourCC_video_type_RGBA;
            ndi_frame.line_stride_in_bytes = dim.x * 4;
            ndi_frame.p_data               = ndi_sdk::send_buffer(frame_bytes);
            ndi_frame.frame_rate_N         = frame_rate.numerator;
            ndi_frame.frame_rate_D         = frame_rate.denominator;
            ndi_frame.frame_format_type    = NDIlib_frame_format_type_progressive;
            ndi_frame.timecode             = pts.count() * 10'000'000LL / utils::k_flicks_one_second.count();
            ndi_frame.p_metadata           = COLOR_METADATA;

            // May block briefly to fence the previous async send.
            // This is the only blocking point, entirely off the main thread.
            NDIlib_send_send_video_async_v2(sender_, &ndi_frame);

            if (inflight.has_value()) {
                inflight.reset();
            }

            inflight = std::move(*frame);
        }

        NDIlib_send_send_video_async_v2(sender_, nullptr);
        inflight.reset();
    }

    bool update_frame_rate(utils::flicks duration)
    {
        if (!frame_duration_.observe(duration)) {
            return frame_rate_.load(std::memory_order_relaxed).denominator != 0;
        }

        if (duration <= utils::flicks::zero()) {
            frame_rate_.store({}, std::memory_order_relaxed);
            log()->error("Cannot publish NDI output with a non-positive frame duration");
            return false;
        }

        const auto duration_count = duration.count();
        const auto second_count   = utils::k_flicks_one_second.count();
        const auto divisor        = std::gcd(second_count, duration_count);
        const auto numerator      = second_count / divisor;
        const auto denominator    = duration_count / divisor;

        if (!std::in_range<int>(numerator) || !std::in_range<int>(denominator)) {
            frame_rate_.store({}, std::memory_order_relaxed);
            log()->error("Cannot represent the current frame duration as an NDI frame rate");
            return false;
        }

        frame_rate_.store({.numerator = static_cast<int>(numerator), .denominator = static_cast<int>(denominator)},
                          std::memory_order_relaxed);
        return true;
    }

    void create_sender(const std::string& name)
    {
        NDIlib_send_create_t create{};
        create.p_ndi_name  = name.c_str();
        create.clock_video = false; // render loop is the clock
        create.clock_audio = false;

        sender_ = NDIlib_send_create(&create);
        if (sender_ == nullptr) {
            log()->error("NDIlib_send_create failed for \"{}\"", name);
            return;
        }

        sender_name_.commit(name);
        log()->info("NDI sender created: \"{}\"", name);

        worker_running_ = true;
        worker_thread_  = std::thread(&node_impl::worker_loop, this);
    }

    void free_sender()
    {
        worker_running_ = false;
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        if (sender_ != nullptr) {
            NDIlib_send_destroy(sender_);
            sender_ = nullptr;
        }

        sender_name_.reset();
        {
            const std::scoped_lock lock(cv_mutex_);
            download_stream_.reset();
        }
        stream_dimensions_.reset();
        textured_quad_.reset();
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override { free_sender(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;

        auto*      sr               = app->status_registry();
        const auto enabled          = state.get_option<bool>("enabled");
        const bool frame_rate_valid = update_frame_rate(app->frame_info.duration);

        sr->write(id_, "connected", sender_ != nullptr);

        if (!enabled || !frame_rate_valid) {
            if (sender_ != nullptr) {
                free_sender();
            }
            return;
        }

        const auto source_name  = state.get_option<std::string>("source_name", id_);
        const auto desired_name = source_name.empty() ? id_ : source_name;
        if (sender_ == nullptr || sender_name_.would_change(desired_name)) {
            if (sender_ != nullptr) {
                free_sender();
            }
            create_sender(desired_name);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (sender_ == nullptr) {
            return;
        }

        auto texture = iface_tex_.resolve_value(app, nodes, state);
        if (texture == nullptr) {
            return;
        }

        if (NDIlib_send_get_no_connections(sender_, 0) == 0) {
            return;
        }

        const auto dim = texture->display_dimensions();

        std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream;
        {
            const std::scoped_lock lock(cv_mutex_);
            download_stream = download_stream_;
        }

        if (!download_stream || stream_dimensions_.would_change(dim)) {
            const gpu::transfer::texture_transfer_requirements_s requirements{
                .dimensions  = dim,
                .format      = gpu::texture_s::format_e::rgba_u8,
                .row_stride  = static_cast<size_t>(dim.x) * 4,
                .byte_size   = static_cast<size_t>(dim.x) * static_cast<size_t>(dim.y) * 4,
                .host_access = gpu::transfer::host_access_e::read_only,
            };
            auto stream = app->texture_download_service()->create_stream({
                .requirements = requirements,
                .max_slots    = 7,
            });
            {
                const std::scoped_lock lock(cv_mutex_);
                download_stream_ = stream;
            }
            download_stream = std::move(stream);
            stream_dimensions_.commit(dim);
        }

        auto target = download_stream->try_acquire();
        if (!target) {
            return;
        }

        // Render into an RGBA staging texture with a vertical flip so the raw
        // download has the top-to-bottom row order expected by NDI.
        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::apply_gamma);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        target->framebuffer()->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_->draw(texture);
        gpu::framebuffer_s::end_render();

        target->set_tag(static_cast<uint64_t>(app->frame_info.pts.count()));
        target->submit();
    }

    void complete(core::app_state_s* /*app*/) final { cv_.notify_one(); }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",        "NDI Output"},
            {"enabled",     true        },
            {"source_name", id_         },
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

    std::string_view type() const final { return "ndi_output"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<miximus::nodes::node_i> create_output_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
