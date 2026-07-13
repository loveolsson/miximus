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
#include "utils/flicks.hpp"
#include "utils/frame_queue.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>

namespace {
using namespace miximus;
using namespace miximus::nodes;

auto log() { return getlog("ndi"); }

struct ndi_frame_s
{
    std::unique_ptr<gpu::transfer::transfer_i> transfer;
    gpu::vec2i_t                               dim{};
    utils::flicks                              pts{};

    ndi_frame_s()                                  = default;
    ndi_frame_s(const ndi_frame_s&)                = delete;
    ndi_frame_s& operator=(const ndi_frame_s&)     = delete;
    ndi_frame_s(ndi_frame_s&&) noexcept            = default;
    ndi_frame_s& operator=(ndi_frame_s&&) noexcept = default;
};

class node_impl : public node_i
{
    NDIlib_send_instance_t sender_{nullptr};
    std::string            sender_name_;
    int                    frame_rate_n_{60};
    int                    frame_rate_d_{1};

    std::unique_ptr<gpu::framebuffer_s>              framebuffer_;
    std::unique_ptr<gpu::draw_state_s>               draw_state_;
    std::optional<gpu::transfer::transfer_i::type_e> registered_transfer_type_;

    utils::frame_queue_s<ndi_frame_s> frames_free_;
    utils::frame_queue_s<ndi_frame_s> frames_pending_;

    std::mutex              cv_mutex_;
    std::condition_variable cv_;
    std::atomic<bool>       worker_running_{false};
    std::thread             worker_thread_;

    // Slot prepared in execute(), posted to worker in complete().
    std::optional<ndi_frame_s> execute_slot_;

    input_interface_s<gpu::texture_s*> iface_tex_{"tex"};

    static constexpr int  FREE_FRAME_COUNT = 7;
    static constexpr auto COLOR_METADATA =
        "<ndi_color_info primaries=\"bt_709\" transfer=\"bt_709\" matrix=\"bt_709\"/>";

    // Worker thread: no GL context needed.
    // Ordering guarantee: complete() runs after finish() (node_manager contract),
    // and the queue mutex provides the memory barrier between the main thread's
    // GPU readback and this thread's read of transfer->ptr().
    void worker_loop()
    {
        std::optional<ndi_frame_s> inflight;

        while (worker_running_.load()) {
            {
                std::unique_lock lock(cv_mutex_);
                cv_.wait(lock, [&] { return frames_pending_.size() > 0 || !worker_running_.load(); });
            }

            utils::frame_queue_s<ndi_frame_s>::record_s record;
            if (!frames_pending_.pop_frame(&record)) {
                continue;
            }

            auto slot = std::move(record.frame);

            NDIlib_video_frame_v2_t ndi_frame{};
            ndi_frame.xres                 = slot.dim.x;
            ndi_frame.yres                 = slot.dim.y;
            ndi_frame.FourCC               = NDIlib_FourCC_video_type_BGRA;
            ndi_frame.line_stride_in_bytes = slot.dim.x * 4;
            ndi_frame.p_data               = static_cast<uint8_t*>(slot.transfer->ptr());
            ndi_frame.frame_rate_N         = frame_rate_n_;
            ndi_frame.frame_rate_D         = frame_rate_d_;
            ndi_frame.frame_format_type    = NDIlib_frame_format_type_progressive;
            ndi_frame.timecode             = slot.pts.count() * 10'000'000LL / utils::k_flicks_one_second.count();
            ndi_frame.p_metadata           = COLOR_METADATA;

            // May block briefly to fence the previous async send.
            // This is the only blocking point, entirely off the main thread.
            NDIlib_send_send_video_async_v2(sender_, &ndi_frame);

            if (inflight.has_value()) {
                frames_free_.push_frame(std::move(*inflight));
                inflight.reset();
            }

            inflight = std::move(slot);
        }

        NDIlib_send_send_video_async_v2(sender_, nullptr);
        if (inflight.has_value()) {
            frames_free_.push_frame(std::move(*inflight));
        }
    }

    void create_sender(const std::string& name, core::app_state_s* app)
    {
        const auto dur = app->frame_info.duration.count();
        const auto sec = utils::k_flicks_one_second.count();
        const auto g   = std::gcd(static_cast<int>(sec), static_cast<int>(dur));
        frame_rate_n_  = static_cast<int>(sec) / g;
        frame_rate_d_  = static_cast<int>(dur) / g;

        NDIlib_send_create_t create{};
        create.p_ndi_name  = name.c_str();
        create.clock_video = false; // render loop is the clock
        create.clock_audio = false;

        sender_ = NDIlib_send_create(&create);
        if (sender_ == nullptr) {
            log()->error("NDIlib_send_create failed for \"{}\"", name);
            return;
        }

        sender_name_ = name;
        log()->info("NDI sender created: \"{}\"", name);

        for (int i = 0; i < FREE_FRAME_COUNT; ++i) {
            frames_free_.push_frame(ndi_frame_s{});
        }

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

        if (framebuffer_ && registered_transfer_type_) {
            gpu::transfer::transfer_i::unregister_texture(*registered_transfer_type_, framebuffer_->texture());
            registered_transfer_type_.reset();
        }

        sender_name_.clear();
        execute_slot_.reset();
        framebuffer_.reset();
        draw_state_.reset();
        frames_free_.clear();
        frames_pending_.clear();
    }

  public:
    explicit node_impl() { register_interface(&iface_tex_); }

    ~node_impl() override { free_sender(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;

        auto* sr      = app->status_registry();
        auto  enabled = state.get_option<bool>("enabled");

        sr->write(id_, "connected", sender_ != nullptr);

        if (!enabled) {
            if (sender_ != nullptr) {
                free_sender();
            }
            return;
        }

        const auto source_name  = state.get_option<std::string>("source_name", id_);
        const auto desired_name = source_name.empty() ? id_ : source_name;
        if (sender_ == nullptr || sender_name_ != desired_name) {
            if (sender_ != nullptr) {
                free_sender();
            }
            create_sender(desired_name, app);
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

        if (!framebuffer_ || framebuffer_->texture()->display_dimensions() != dim) {
            if (framebuffer_ && registered_transfer_type_) {
                gpu::transfer::transfer_i::unregister_texture(*registered_transfer_type_, framebuffer_->texture());
            }
            framebuffer_              = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::bgra_u8);
            registered_transfer_type_ = gpu::transfer::transfer_i::get_prefered_type();
            gpu::transfer::transfer_i::register_texture(*registered_transfer_type_, framebuffer_->texture());
        }

        // Non-blocking: drop frame if no free slot available.
        utils::frame_queue_s<ndi_frame_s>::record_s record;
        if (!frames_free_.pop_frame(&record)) {
            return;
        }

        auto slot = std::move(record.frame);

        // Recreate transfer if dimensions changed.
        const size_t frame_size = static_cast<size_t>(dim.x) * dim.y * 4;
        if (slot.dim != dim || !slot.transfer) {
            slot.dim = dim;
            slot.transfer =
                gpu::transfer::transfer_i::create_transfer(gpu::transfer::transfer_i::get_prefered_type(),
                                                           frame_size,
                                                           gpu::transfer::transfer_i::direction_e::gpu_to_cpu);
        }

        // Allow GL to render: for DVP, waits for pending DMA reads of this texture to complete.
        gpu::transfer::transfer_i::begin_texture_use(slot.transfer->type(), framebuffer_->texture());

        slot.pts = app->frame_info.pts;

        // Blit input texture → bgra_u8 framebuffer with vertical flip so that
        // glReadPixels (bottom-to-top) produces top-to-bottom NDI data.
        if (!draw_state_) {
            draw_state_ = std::make_unique<gpu::draw_state_s>();
            auto shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::apply_gamma);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);

        auto shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", gpu::vec2_t{0.0, 0.0});
        shader->set_uniform("scale", gpu::vec2_t{1.0, 1.0});
        shader->set_uniform("opacity", 1.0);

        texture->bind(0);
        draw_state_->draw();
        gpu::texture_s::unbind(0);

        // Async readback: transfer reads from the currently-bound framebuffer into
        // its PBO. finish() (called by node_manager after all execute() calls)
        // guarantees completion before complete() posts to the worker.
        slot.transfer->perform_transfer(framebuffer_.get());
        // perform_transfer handles the glMemoryBarrier and (for persistent) the sync.
        // No separate glBindBuffer / glReadPixels / glBindBuffer needed.

        gpu::framebuffer_s::end_render();

        execute_slot_ = std::move(slot);
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (!execute_slot_.has_value() || sender_ == nullptr) {
            execute_slot_.reset();
            return;
        }

        // finish() has been called. Complete any backend-specific asynchronous
        // readback before exposing the host buffer to the NDI worker.
        execute_slot_->transfer->wait_for_copy();
        frames_pending_.push_frame(std::move(*execute_slot_));
        execute_slot_.reset();
        cv_.notify_one();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", std::format("NDI Output {}", id_)},
            {"enabled", true},
            {"source_name", ""},
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

    std::string_view type() const final { return "ndi_output"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<miximus::nodes::node_i> create_output_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
