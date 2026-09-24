#include "capture_stream.hpp"

#include "gpu/detail/dma_buf_copy.hpp"

namespace miximus::nodes::cef::detail {

using namespace std::chrono_literals;

capture_stream_s::capture_stream_s(gpu::device_s& device, gpu::vec2i_t dimensions, int frame_rate)
    : dimensions_(dimensions)
    , frame_rate_(frame_rate)
    , pool_(device, dimensions, FRAME_CAPACITY, FRAME_BUDGET)
    , context_(device.create_recording_context(1))
{
}

size_t capture_stream_s::texture_budget(gpu::vec2i_t dimensions)
{
    if (dimensions.x < 1 || dimensions.y < 1 || dimensions.x > 8192 || dimensions.y > 8192) {
        throw std::invalid_argument("Invalid CEF viewport dimensions");
    }
    const auto bytes = frame_pool_s::storage_bytes(dimensions);
    if (bytes == 0 || bytes > FRAME_BUDGET / FRAME_CAPACITY) {
        throw std::invalid_argument("CEF viewport exceeds the session texture budget");
    }
    return bytes * FRAME_CAPACITY;
}

bool capture_stream_s::capture(const CefAcceleratedPaintInfo& info, const std::atomic_bool& close_requested)
{
    const auto capture_started = std::chrono::steady_clock::now();
    const auto arrival         = utils::flicks_now();
    const auto sequence        = ++received;
    try {
        if (info.plane_count != 1 ||
            (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888)) {
            throw std::runtime_error("Unsupported CEF accelerated format or plane count");
        }
        if (info.extra.coded_size.width != dimensions_.x || info.extra.coded_size.height != dimensions_.y) {
            throw std::runtime_error("CEF paint does not match the session viewport generation");
        }
        const auto maximum_timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(utils::flicks::max()).count();
        if (info.extra.timestamp > static_cast<uint64_t>(maximum_timestamp)) {
            throw std::runtime_error("CEF capture timestamp is outside the supported range");
        }
        if (previous_timestamp_ && info.extra.timestamp < *previous_timestamp_) {
            ++epoch_;
        }
        previous_timestamp_ = info.extra.timestamp;

        auto frame     = pool_.try_acquire();
        auto recording = frame ? context_.try_record() : nullptr;
        if (!frame || !recording) {
            ++dropped;
            return false;
        }
        gpu::detail::dma_buf_image_s source;
        source.fd     = info.planes[0].fd;
        source.extent = {.width  = static_cast<uint32_t>(info.extra.coded_size.width),
                         .height = static_cast<uint32_t>(info.extra.coded_size.height)};
        source.order =
            info.format == CEF_COLOR_TYPE_BGRA_8888 ? gpu::channel_order_e::bgra : gpu::channel_order_e::rgba;
        source.modifier = info.modifier;
        source.offset   = info.planes[0].offset;
        source.stride   = info.planes[0].stride;
        gpu::draw_s conversion;
        conversion.compositing = gpu::compositing_e::replace;
        conversion.transfer    = gpu::color_operation_e::decode_srgb_premultiplied;
        const auto completion =
            gpu::detail::dma_buf_copy_s::submit(*recording, source, frame->texture(), conversion, 100ms);
        // A timeout does not cancel GPU work. The borrowed source cannot be
        // returned while our read is pending; the app's shutdown watchdog
        // handles a device that stops making progress.
        const auto wait_started = std::chrono::steady_clock::now();
        while (completion.wait(1s) != gpu::wait_result_e::ready) {
        }
        const auto wait_finished = std::chrono::steady_clock::now();
        if (close_requested) {
            return false;
        }
        const media::media_clock_sample_s clock{
            .stream_epoch   = epoch_,
            .frame_sequence = sequence,
            .media_pts      = utils::flicks_cast(std::chrono::microseconds(info.extra.timestamp)),
            .frame_duration = utils::k_flicks_one_second / frame_rate_,
        };
        frames.push(std::make_shared<frame_queue_t::frame_t>(
            clock, arrival, std::move(frame), media::source_frame_readiness_e::ready));
        {
            const auto             finished = std::chrono::steady_clock::now();
            const std::scoped_lock lock(mutex);
            capture_timing.add(finished - capture_started);
            completion_wait_timing.add(wait_finished - wait_started);
            ++copied;
        }
        return true;
    } catch (const gpu::recording_unavailable_s&) {
        ++dropped;
        return false;
    }
}

void capture_stream_s::advance_frames(utils::flicks pts, utils::flicks target_time, bool discontinuity)
{
    frames.advance(pts, target_time, discontinuity);
}

bool capture_stream_s::submit_frame(utils::flicks pts)
{
    prepared.emplace(frames.select_nearest(pts));
    return prepared->frame() != nullptr;
}

capture_stream_s::frame_ptr_t capture_stream_s::resolve_frame()
{
    if (!prepared || !prepared->frame()) {
        return {};
    }
    const auto& ticket = *prepared;
    if (!ticket.await() || !frames.commit(ticket)) {
        return {};
    }
    return ticket.frame()->payload();
}

void capture_stream_s::release_prepared_frame() { prepared.reset(); }
void capture_stream_s::reset_frames()
{
    release_prepared_frame();
    frames.reset();
}

session_s::metrics_s capture_stream_s::metrics() const
{
    const std::scoped_lock lock(mutex);
    session_s::metrics_s   result{};
    result.received        = received.load();
    result.copied          = copied.load();
    result.dropped         = dropped.load();
    result.capture         = capture_timing.snapshot();
    result.completion_wait = completion_wait_timing.snapshot();
    result.source_queue    = frames.metrics();
    return result;
}
} // namespace miximus::nodes::cef::detail
