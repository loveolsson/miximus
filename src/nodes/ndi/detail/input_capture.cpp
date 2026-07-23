#include "input_capture.hpp"

#include "gpu/transfer/texture_upload.hpp"
#include "logger/logger.hpp"
#include "types/frame_rate.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace miximus::nodes::ndi::detail {
namespace {
auto log() { return getlog("ndi"); }

constexpr size_t SOURCE_QUEUE_CAPACITY = 4;
constexpr size_t UPLOAD_SLOT_COUNT     = 8;
constexpr auto   CAPTURE_TIMEOUT       = std::chrono::milliseconds(50);

using ndi_ticks_t = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;

struct source_format_s
{
    gpu::vec2i_t dimensions;
    frame_rate_s frame_rate;

    bool operator==(const source_format_s& other) const
    {
        return dimensions == other.dimensions && frame_rate == other.frame_rate;
    }
};

struct captured_frame_data_s
{
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    std::optional<gpu::transfer::texture_upload_lease_s>    upload;
    uint64_t                                                upload_version{};
    gpu::texture_s*                                         texture{};
    gpu::vec2i_t                                            dimensions{};
    int64_t                                                 ndi_timecode{};
    int64_t                                                 ndi_timestamp{NDIlib_recv_timestamp_undefined};
};

bool copy_frame(const NDIlib_video_frame_v2_t& video_frame, std::span<std::byte> destination, size_t row_size)
{
    const auto frame_size = row_size * static_cast<size_t>(video_frame.yres);
    const auto source_stride =
        video_frame.line_stride_in_bytes == 0 ? row_size : static_cast<size_t>(video_frame.line_stride_in_bytes);
    if (destination.size() < frame_size || video_frame.line_stride_in_bytes < 0 || source_stride < row_size) {
        return false;
    }

    auto* source = video_frame.p_data;
    if (source_stride == row_size) {
        std::memcpy(destination.data(), source, frame_size);
        return true;
    }

    for (int y = 0; y < video_frame.yres; ++y) {
        const auto offset = static_cast<size_t>(y) * row_size;
        std::memcpy(destination.data() + offset, source, row_size);
        source += source_stride;
    }
    return true;
}
} // namespace

class input_capture_s::impl_s
{
    using frame_queue_t  = media::timed_source_queue_s<captured_frame_data_s>;
    using frame_ticket_t = frame_queue_t::ticket_t;

    gpu::transfer::texture_upload_service_s* upload_service_;
    utils::serial_executor_s*                control_executor_;
    std::string                              source_name_;
    std::string                              receiver_name_;

    NDIlib_recv_instance_t receiver_{nullptr};

    std::atomic<phase_e> phase_{phase_e::starting};
    std::atomic_bool     stop_requested_{};
    std::atomic_bool     worker_running_{};
    std::thread          worker_;

    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    gpu::vec2i_t                                            upload_dimensions_{};

    frame_queue_t frame_queue_{
        {.capacity = SOURCE_QUEUE_CAPACITY, .playout_delay_frames = 1}
    };
    std::optional<frame_ticket_t> prepared_frame_;

    std::optional<source_format_s> source_format_;
    std::optional<bool>            source_has_timestamp_;
    std::optional<int64_t>         timestamp_origin_;
    std::optional<int64_t>         previous_timestamp_;
    uint64_t                       source_epoch_{1};
    uint64_t                       next_sequence_{};

    std::atomic_uint64_t                  frames_received_{};
    std::atomic_uint64_t                  invalid_frames_{};
    std::atomic_uint64_t                  receiver_video_drops_{};
    std::atomic_uint32_t                  receiver_queue_depth_{};
    std::atomic_uint64_t                  upload_slot_drops_{};
    std::chrono::steady_clock::time_point next_metrics_poll_;

    bool warned_invalid_frame_{};
    bool warned_submit_failure_{};
    bool warned_wait_failure_{};
    bool warned_consume_failure_{};
    bool warned_commit_failure_{};

    void post_control(std::shared_ptr<input_capture_s> owner, void (impl_s::*task)())
    {
        if (!control_executor_->post([owner = std::move(owner), task] { (owner->impl_.get()->*task)(); })) {
            phase_ = phase_e::failed;
        }
    }

    std::shared_ptr<gpu::transfer::texture_upload_stream_s>
    get_upload_stream(gpu::vec2i_t dimensions, size_t row_stride, size_t frame_size)
    {
        if (upload_stream_ && upload_dimensions_ == dimensions) {
            return upload_stream_;
        }

        const gpu::transfer::texture_transfer_requirements_s requirements{
            .dimensions  = dimensions,
            .format      = gpu::texture_s::format_e::bgra_u8,
            .row_stride  = row_stride,
            .byte_size   = frame_size,
            .host_access = gpu::transfer::host_access_e::overwrite,
        };
        upload_stream_     = upload_service_->create_stream({
                .requirements      = requirements,
                .max_slots         = UPLOAD_SLOT_COUNT,
                .generate_mip_maps = false,
        });
        upload_dimensions_ = dimensions;
        return upload_stream_;
    }

    void begin_source_epoch(source_format_s format, bool has_timestamp, std::optional<int64_t> timestamp)
    {
        if (source_format_.has_value()) {
            ++source_epoch_;
        }
        source_format_        = format;
        source_has_timestamp_ = has_timestamp;
        timestamp_origin_     = timestamp;
        previous_timestamp_   = timestamp;
        next_sequence_        = 0;
    }

    std::optional<media::media_frame_id_s>
    make_frame_id(const NDIlib_video_frame_v2_t& video_frame, source_format_s format, utils::flicks duration)
    {
        const bool has_timestamp =
            video_frame.timestamp != NDIlib_recv_timestamp_undefined && video_frame.timestamp >= 0;
        const bool format_changed = source_format_.has_value() && *source_format_ != format;
        const bool timestamp_mode_changed =
            source_has_timestamp_.has_value() && *source_has_timestamp_ != has_timestamp;
        const bool timestamp_regressed =
            has_timestamp && previous_timestamp_.has_value() && video_frame.timestamp <= *previous_timestamp_;

        if (!source_format_.has_value() || format_changed || timestamp_mode_changed || timestamp_regressed) {
            begin_source_epoch(
                format, has_timestamp, has_timestamp ? std::optional(video_frame.timestamp) : std::nullopt);
        }

        utils::flicks source_pts;
        if (has_timestamp) {
            if (!timestamp_origin_.has_value() || video_frame.timestamp < *timestamp_origin_) {
                return std::nullopt;
            }
            source_pts =
                std::chrono::duration_cast<utils::flicks>(ndi_ticks_t{video_frame.timestamp - *timestamp_origin_});
            previous_timestamp_ = video_frame.timestamp;
        } else {
            source_pts = duration * static_cast<utils::flicks::rep>(next_sequence_);
        }

        return media::media_frame_id_s{
            .epoch    = source_epoch_,
            .sequence = next_sequence_++,
            .pts      = source_pts,
            .duration = duration,
        };
    }

    bool process_video_frame(const NDIlib_video_frame_v2_t& video_frame, utils::flicks arrival_time)
    {
        if (video_frame.p_data == nullptr || video_frame.xres <= 0 || video_frame.yres <= 0 ||
            video_frame.frame_rate_N <= 0 || video_frame.frame_rate_D <= 0 ||
            (video_frame.FourCC != NDIlib_FourCC_video_type_BGRA &&
             video_frame.FourCC != NDIlib_FourCC_video_type_BGRX)) {
            return false;
        }

        const auto frame_rate = frame_rate_s{
            .numerator   = static_cast<uint32_t>(video_frame.frame_rate_N),
            .denominator = static_cast<uint32_t>(video_frame.frame_rate_D),
        };
        const auto duration = get_frame_duration(frame_rate);
        if (!duration.has_value()) {
            return false;
        }

        const gpu::vec2i_t dimensions{video_frame.xres, video_frame.yres};
        const auto         row_size   = static_cast<size_t>(video_frame.xres) * 4;
        const auto         frame_size = row_size * static_cast<size_t>(video_frame.yres);
        auto               stream     = get_upload_stream(dimensions, row_size, frame_size);
        auto               upload     = stream->try_acquire();
        if (!upload.has_value()) {
            ++upload_slot_drops_;
            return true;
        }

        if (!copy_frame(video_frame, upload->bytes(), row_size)) {
            return false;
        }

        auto frame_id = make_frame_id(video_frame, {dimensions, frame_rate}, *duration);
        if (!frame_id.has_value()) {
            return false;
        }

        const auto upload_version = upload->version();
        frame_queue_.push(frame_queue_.create_frame(*frame_id,
                                                    arrival_time,
                                                    captured_frame_data_s{
                                                        .stream         = std::move(stream),
                                                        .upload         = std::move(upload),
                                                        .upload_version = upload_version,
                                                        .texture        = nullptr,
                                                        .dimensions     = dimensions,
                                                        .ndi_timecode   = video_frame.timecode,
                                                        .ndi_timestamp  = video_frame.timestamp,
                                                    }));
        return true;
    }

    void poll_receiver_metrics()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_metrics_poll_) {
            return;
        }

        NDIlib_recv_performance_t total{};
        NDIlib_recv_performance_t dropped{};
        NDIlib_recv_get_performance(receiver_, &total, &dropped);
        receiver_video_drops_ = dropped.video_frames < 0 ? 0 : static_cast<uint64_t>(dropped.video_frames);

        NDIlib_recv_queue_t queue{};
        NDIlib_recv_get_queue(receiver_, &queue);
        receiver_queue_depth_ = queue.video_frames < 0 ? 0 : static_cast<uint32_t>(queue.video_frames);
        next_metrics_poll_    = now + std::chrono::seconds(1);
    }

    void capture_loop()
    {
        while (worker_running_.load()) {
            NDIlib_video_frame_v2_t video_frame{};
            const auto              frame_type = NDIlib_recv_capture_v3(
                receiver_, &video_frame, nullptr, nullptr, static_cast<uint32_t>(CAPTURE_TIMEOUT.count()));
            const auto arrival_time = utils::flicks_now();

            if (frame_type == NDIlib_frame_type_video) {
                ++frames_received_;
                try {
                    if (!process_video_frame(video_frame, arrival_time)) {
                        ++invalid_frames_;
                        if (!warned_invalid_frame_) {
                            log()->warn("NDI input received an unsupported or invalid video frame from \"{}\"",
                                        source_name_);
                            warned_invalid_frame_ = true;
                        }
                    }
                } catch (const std::exception& error) {
                    NDIlib_recv_free_video_v2(receiver_, &video_frame);
                    log()->error("NDI input capture failed for \"{}\": {}", source_name_, error.what());
                    phase_          = phase_e::failed;
                    worker_running_ = false;
                    return;
                } catch (...) {
                    NDIlib_recv_free_video_v2(receiver_, &video_frame);
                    log()->error("NDI input capture failed for \"{}\"", source_name_);
                    phase_          = phase_e::failed;
                    worker_running_ = false;
                    return;
                }
                NDIlib_recv_free_video_v2(receiver_, &video_frame);
            } else if (frame_type == NDIlib_frame_type_error) {
                log()->error("NDI receiver reported a connection error for \"{}\"", source_name_);
                phase_          = phase_e::failed;
                worker_running_ = false;
                return;
            }

            poll_receiver_metrics();
        }
    }

  public:
    impl_s(gpu::transfer::texture_upload_service_s* upload_service,
           utils::serial_executor_s*                control_executor,
           std::string                              source_name,
           std::string                              receiver_name)
        : upload_service_(upload_service)
        , control_executor_(control_executor)
        , source_name_(std::move(source_name))
        , receiver_name_(std::move(receiver_name))
    {
    }

    ~impl_s() { stop_control(); }

    void start_control()
    {
        if (stop_requested_.load()) {
            phase_ = phase_e::stopped;
            return;
        }

        NDIlib_recv_create_v3_t create{};
        create.color_format       = NDIlib_recv_color_format_BGRX_BGRA;
        create.bandwidth          = NDIlib_recv_bandwidth_highest;
        create.allow_video_fields = false;
        create.p_ndi_recv_name    = receiver_name_.c_str();

        NDIlib_source_t source{};
        source.p_ndi_name           = source_name_.c_str();
        create.source_to_connect_to = source;

        receiver_ = NDIlib_recv_create_v3(&create);
        if (receiver_ == nullptr) {
            log()->error("NDIlib_recv_create_v3 failed for \"{}\"", source_name_);
            phase_ = phase_e::failed;
            return;
        }

        worker_running_ = true;
        worker_         = std::thread(&impl_s::capture_loop, this);
        phase_          = phase_e::running;
        log()->info("NDI input capture running: \"{}\"", source_name_);
    }

    void stop_control()
    {
        const auto previous = phase_.exchange(phase_e::stopping);
        if (previous == phase_e::stopped) {
            phase_ = phase_e::stopped;
            return;
        }

        worker_running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }

        if (receiver_ != nullptr) {
            NDIlib_recv_destroy(receiver_);
            receiver_ = nullptr;
        }
        upload_stream_.reset();
        upload_dimensions_ = {};
        phase_             = phase_e::stopped;
    }

    void request_stop() { stop_requested_ = true; }

    phase_e phase() const { return phase_.load(); }

    input_capture_s::metrics_s metrics() const
    {
        return {
            .frames_received      = frames_received_.load(),
            .invalid_frames       = invalid_frames_.load(),
            .receiver_video_drops = receiver_video_drops_.load(),
            .receiver_queue_depth = receiver_queue_depth_.load(),
            .upload_slot_drops    = upload_slot_drops_.load(),
            .source_queue         = frame_queue_.metrics(),
        };
    }

    void advance_frames(utils::flicks program_pts, utils::flicks target_time, bool discontinuity)
    {
        frame_queue_.advance(program_pts, target_time, discontinuity);
    }

    bool submit_frame(utils::flicks program_pts)
    {
        prepared_frame_.reset();
        if (phase() != phase_e::running) {
            return false;
        }

        prepared_frame_.emplace(frame_queue_.select(program_pts));
        auto& ticket = *prepared_frame_;
        if (ticket.selection() == media::prepared_frame_selection_e::repeat) {
            return true;
        }
        if (ticket.selection() != media::prepared_frame_selection_e::new_frame || ticket.frame() == nullptr) {
            prepared_frame_.reset();
            return false;
        }

        auto& frame = *ticket.frame();
        auto& info  = frame.value();
        if (frame.readiness() == media::source_frame_readiness_e::submitted) {
            return true;
        }
        if (!frame.mark_submitted() || !info.upload.has_value() || !info.upload->submit()) {
            if (!warned_submit_failure_) {
                log()->warn("NDI timed input failed to submit upload version {}", info.upload_version);
                warned_submit_failure_ = true;
            }
            frame_queue_.fail(ticket);
            prepared_frame_.reset();
            return false;
        }
        info.upload.reset();
        return true;
    }

    std::optional<resolved_input_frame_s> resolve_frame()
    {
        if (!prepared_frame_.has_value() || prepared_frame_->frame() == nullptr) {
            return std::nullopt;
        }

        auto& ticket = *prepared_frame_;
        auto& frame  = *ticket.frame();
        auto& info   = frame.value();
        if (ticket.selection() == media::prepared_frame_selection_e::new_frame) {
            const auto wait_result = info.stream ? info.stream->wait_until_ready(info.upload_version)
                                                 : gpu::transfer::texture_upload_wait_result_e::stopped;
            if (wait_result != gpu::transfer::texture_upload_wait_result_e::ready) {
                if (!warned_wait_failure_) {
                    log()->warn("NDI timed input failed waiting for upload version {}", info.upload_version);
                    warned_wait_failure_ = true;
                }
                frame_queue_.fail(ticket);
                return std::nullopt;
            }

            info.texture = info.stream->consume_exact(info.upload_version);
            if (info.texture == nullptr || info.stream->current_version() != info.upload_version ||
                !frame.mark_ready()) {
                if (!warned_consume_failure_) {
                    log()->warn("NDI timed input failed to consume upload version {}", info.upload_version);
                    warned_consume_failure_ = true;
                }
                frame_queue_.fail(ticket);
                return std::nullopt;
            }
        }

        if (!ticket.await() || !frame_queue_.commit(ticket)) {
            if (!warned_commit_failure_) {
                log()->warn("NDI timed input failed to commit upload version {}", info.upload_version);
                warned_commit_failure_ = true;
            }
            return std::nullopt;
        }
        return resolved_input_frame_s{
            .texture    = info.texture,
            .dimensions = info.dimensions,
        };
    }

    void release_prepared_frame()
    {
        if (prepared_frame_.has_value() &&
            prepared_frame_->selection() == media::prepared_frame_selection_e::new_frame &&
            prepared_frame_->frame() != nullptr &&
            prepared_frame_->frame()->readiness() == media::source_frame_readiness_e::submitted) {
            auto& info = prepared_frame_->frame()->value();
            if (info.stream) {
                info.stream->discard_exact(info.upload_version);
            }
            frame_queue_.fail(*prepared_frame_);
        }
        prepared_frame_.reset();
    }

    void reset_frames()
    {
        release_prepared_frame();
        frame_queue_.reset();
    }

    friend class input_capture_s;
};

input_capture_s::input_capture_s(gpu::transfer::texture_upload_service_s* upload_service,
                                 utils::serial_executor_s*                control_executor,
                                 std::string                              source_name,
                                 std::string                              receiver_name)
    : impl_(
          std::make_unique<impl_s>(upload_service, control_executor, std::move(source_name), std::move(receiver_name)))
{
}

std::shared_ptr<input_capture_s> input_capture_s::create(gpu::transfer::texture_upload_service_s* upload_service,
                                                         utils::serial_executor_s*                control_executor,
                                                         std::string                              source_name,
                                                         std::string                              receiver_name)
{
    return std::shared_ptr<input_capture_s>(
        new input_capture_s(upload_service, control_executor, std::move(source_name), std::move(receiver_name)));
}

input_capture_s::~input_capture_s() = default;

void input_capture_s::start_async() { impl_->post_control(shared_from_this(), &impl_s::start_control); }

void input_capture_s::stop_async()
{
    impl_->request_stop();
    impl_->post_control(shared_from_this(), &impl_s::stop_control);
}

input_capture_s::phase_e input_capture_s::phase() const { return impl_->phase(); }

input_capture_s::metrics_s input_capture_s::metrics() const { return impl_->metrics(); }

void input_capture_s::advance_frames(utils::flicks program_pts, utils::flicks target_time, bool discontinuity)
{
    impl_->advance_frames(program_pts, target_time, discontinuity);
}

bool input_capture_s::submit_frame(utils::flicks program_pts) { return impl_->submit_frame(program_pts); }

std::optional<resolved_input_frame_s> input_capture_s::resolve_frame() { return impl_->resolve_frame(); }

void input_capture_s::release_prepared_frame() { impl_->release_prepared_frame(); }

void input_capture_s::reset_frames() { impl_->reset_frames(); }

} // namespace miximus::nodes::ndi::detail
