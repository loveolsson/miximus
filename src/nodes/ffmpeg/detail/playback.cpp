#include "playback.hpp"

#include "decoder.hpp"
#include "gpu/detail/fatal.hpp"
#include "logger/logger.hpp"
#include "platform_ingest.hpp"
#include "texture_pool.hpp"
#include "types/buffer_limits.hpp"
#include "utils/failure_shutdown.hpp"
#include "vulkan_ingest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace miximus::nodes::ffmpeg::detail {

using namespace std::chrono_literals;

struct playback_session_s::impl_s
{
    std::shared_ptr<const hardware_devices_s>               devices;
    gpu::device_s&                                          device;
    std::shared_ptr<gpu::transfer::detail::memory_budget_s> gpu_budget;
    size_t                                                  gpu_reserved{};
    size_t                                                  gpu_buffer_reserved{};
    std::vector<std::shared_ptr<gpu::texture_s>>            gpu_pool;

    gpu::transfer::texture_upload_service_s&      uploads;
    const playback_options_s                      options;
    mutable std::mutex                            mutex;
    std::condition_variable                       changed;
    std::atomic_bool                              stopped;
    std::thread                                   worker;
    uint64_t                                      generation{};
    utils::flicks                                 position{};
    std::deque<std::shared_ptr<playback_frame_s>> queue;
    std::shared_ptr<playback_frame_s>             current;
    playback_status_s                             metrics;

    impl_s(gpu::device_s&                                          gpu,
           gpu::transfer::texture_upload_service_s&                service,
           std::shared_ptr<gpu::transfer::detail::memory_budget_s> budget,
           playback_options_s                                      settings,
           std::shared_ptr<const hardware_devices_s>               hardware)
        : devices(std::move(hardware))
        , device(gpu)
        , gpu_budget(std::move(budget))
        , uploads(service)
        , options(std::move(settings))
    {
        validate_input_buffer_frames(options.buffered_frames);
    }

    impl_s(const impl_s& other)            = delete;
    impl_s& operator=(const impl_s& other) = delete;
    impl_s(impl_s&& other)                 = delete;
    impl_s& operator=(impl_s&& other)      = delete;

    ~impl_s()
    {
        // Session destruction/retirement is on the control worker, never rendering.
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::ranges::any_of(gpu_pool, [](const auto& texture) { return !texture->idle(); })) {
            if (std::chrono::steady_clock::now() > deadline) {
                gpu::detail::fatal_gpu_error("Playback texture retirement timed out");
            }

            std::this_thread::sleep_for(1ms);
        }

        gpu_pool.clear();
        gpu_budget->release_memory(gpu_reserved);
    }

    std::shared_ptr<gpu::texture_s> acquire_gpu_texture(gpu::vec2i_t dimensions)
    {
        for (const auto& texture : gpu_pool) {
            if (texture.use_count() == 1 && texture->idle() && texture->dimensions() == dimensions) {
                return texture;
            }
        }

        // Resolution changes must not leave every bounded slot occupied by
        // obsolete geometry. Selected or in-flight textures remain retained.
        retire_unused_textures(gpu_pool, *gpu_budget, gpu_reserved, dimensions);
        if (gpu_pool.size() >= (static_cast<size_t>(options.buffered_frames) + 4)) {
            return {};
        }

        const auto bytes = gpu::texture_s::estimate_storage_byte_size(dimensions, gpu::format_e::rgba_unorm16);
        if (!gpu_budget->reserve_memory(bytes)) {
            throw std::runtime_error("Playback GPU memory budget exhausted");
        }

        try {
            auto texture = std::make_shared<gpu::texture_s>(device, dimensions, gpu::format_e::rgba_unorm16);
            gpu_pool.push_back(texture);
            gpu_reserved += bytes;
            return texture;
        } catch (...) {
            gpu_budget->release_memory(bytes);
            throw;
        }
    }

    void stop()
    {
        stopped = true;
        changed.notify_all();
    }

    void join()
    {
        stop();
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Accessed only by the dedicated decode worker.
    struct worker_state_s
    {
        std::unique_ptr<decoder_s>                              decoder;
        std::unique_ptr<vulkan_ingest_s>                        ingress;
        std::unique_ptr<platform_ingest_s>                      platform_ingress;
        std::vector<std::string>                                rejected_backends;
        bool                                                    hardware_retry{};
        uint64_t                                                active_generation{};
        utils::flicks                                           loop_offset{};
        utils::flicks                                           loop_end{};
        bool                                                    frames_in_loop{};
        std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
        gpu::vec2i_t                                            dimensions{};
        size_t                                                  buffer_bytes{};
    };

    void report_decoder(const decoder_s& decoder)
    {
        std::scoped_lock lock(mutex);
        metrics.backend       = decoder.backend();
        metrics.transfer_path = metrics.backend == "software" ? "host upload" : "qualifying GPU surface sharing";
        if (metrics.fallback_reason.empty()) {
            metrics.fallback_reason = decoder.fallback_reason();
        }
    }

    std::shared_ptr<gpu::texture_s> convert_gpu(worker_state_s& state, decoded_frame_s& decoded)
    {
        std::shared_ptr<gpu::texture_s> gpu_texture;
        if (decoded.hardware_frame()) {
            decoded.prepare_gpu();
            const auto storage_bytes = decoded.gpu_storage_bytes();
            if (storage_bytes > gpu_buffer_reserved) {
                const auto extra = storage_bytes - gpu_buffer_reserved;
                if (!gpu_budget->reserve_memory(extra)) {
                    throw std::runtime_error("Playback GPU buffer budget exhausted");
                }

                gpu_reserved += extra;
                gpu_buffer_reserved = storage_bytes;
            }

            gpu_texture = acquire_gpu_texture(decoded.dimensions);
            if (!gpu_texture) {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, 2ms);
                ++metrics.dropped_frames;
                return {};
            }

            if (decoded.vulkan_frame()) {
                if (!state.ingress) {
                    state.ingress = std::make_unique<vulkan_ingest_s>(device);
                }
                state.ingress->convert(decoded, *gpu_texture);
            } else {
                if (!state.platform_ingress) {
                    state.platform_ingress = std::make_unique<platform_ingest_s>(device);
                }
                state.platform_ingress->convert(decoded, *gpu_texture);
            }
            std::scoped_lock lock(mutex);
            metrics.transfer_path =
                decoded.vulkan_frame() ? "Vulkan device-local components" : platform_ingest_s::transfer_path();
        }

        return gpu_texture;
    }

    // Called with mutex held, only after a usable frame has been queued.
    void mark_ready()
    {
        if (!metrics.error.empty()) {
            getlog("ffmpeg")->info("Video '{}' recovered", options.filename);
            metrics.error.clear();
        }
        metrics.ready = true;
    }

    void publish_decoded(worker_state_s& state, decoded_frame_s& decoded, utils::flicks pts)
    {
        std::shared_ptr<gpu::texture_s> gpu_texture;
        gpu_texture = convert_gpu(state, decoded);
        if (!gpu_texture && decoded.hardware_frame()) {
            return;
        }

        if (!gpu_texture) {
            decoded.prepare_host();
        }

        if (gpu_texture) {
            auto frame         = std::make_shared<playback_frame_s>();
            frame->gpu_texture = std::move(gpu_texture);
            frame->pts         = pts;
            frame->duration    = decoded.duration;
            std::scoped_lock lock(mutex);
            if (generation == state.active_generation) {
                queue.push_back(std::move(frame));
                mark_ready();
            }

            return;
        }

        if (!state.stream || state.dimensions != decoded.dimensions || state.buffer_bytes != decoded.buffer_bytes) {
            state.dimensions   = decoded.dimensions;
            state.buffer_bytes = decoded.buffer_bytes;
            state.stream       = uploads.create_stream({
                .host_layout         = {.image_dimensions  = state.dimensions,
                                        .pixel_format      = gpu::transfer::host_pixel_format_e::video_components,
                                        .buffer_size_bytes = state.buffer_bytes},
                .max_slots           = static_cast<size_t>(options.buffered_frames) + 4,
                .generate_mip_maps   = false,
                .conversion_sampling = gpu::sampling_e::mipmapped_linear,
            });
        }

        std::optional<gpu::transfer::texture_upload_lease_s> upload;
        while (!stopped && !(upload = state.stream->acquire_upload_buffer_for(20ms))) {
            if (state.stream->allocation_failed()) {
                throw std::runtime_error("Video transfer memory budget exhausted");
            }

            std::scoped_lock lock(mutex);
            if (generation != state.active_generation || pts + decoded.duration <= position) {
                break;
            }
        }

        if (stopped) {
            return;
        }

        if (!upload) {
            return;
        }

        decoded.copy_to(upload->writable_host_bytes());
        auto frame       = std::make_shared<playback_frame_s>();
        frame->stream    = state.stream;
        frame->upload_id = upload->upload_id();
        frame->upload    = std::move(*upload);
        frame->color     = decoded.color;
        frame->pts       = pts;
        frame->duration  = decoded.duration;
        if (!frame->upload.submit(gpu::transfer::upload_ownership_e::queued_frame)) {
            throw std::runtime_error("Video transfer submission failed");
        }

        {
            std::scoped_lock lock(mutex);
            if (generation != state.active_generation) {
                return;
            }

            queue.push_back(std::move(frame));
            mark_ready();
        }
    }

    bool wait_for_decode_slot(worker_state_s& state)
    {
        if (!state.hardware_retry) {
            retire_unused_textures(gpu_pool, *gpu_budget, gpu_reserved);
        }

        {
            std::unique_lock lock(mutex);
            const auto       can_decode = [&] {
                return stopped || generation != state.active_generation ||
                       queue.size() < static_cast<size_t>(options.buffered_frames);
            };
            if (!state.hardware_retry && !gpu_pool.empty()) {
                // A paused/full software queue must still release retired GPU
                // storage after render references or submissions finish.
                if (!changed.wait_for(lock, 20ms, can_decode)) {
                    return false;
                }
            } else {
                changed.wait(lock, can_decode);
            }
            if (stopped) {
                return false;
            }

            if (generation != state.active_generation) {
                state.active_generation = generation;
                lock.unlock();
                state.decoder->rewind();
                state.loop_offset = state.loop_end = {};
                state.frames_in_loop               = false;
            }
        }

        return true;
    }

    void decode_iteration(worker_state_s& state)
    {
        if (!wait_for_decode_slot(state)) {
            return;
        }

        std::unique_ptr<decoded_frame_s> decoded;
        try {
            decoded = state.decoder->next();
        } catch (const source_error_s&) {
            throw;
        } catch (const std::exception& error) {
            if (stopped) {
                return;
            }

            if (!state.hardware_retry) {
                throw;
            }

            retry_decoder(state, error.what());
            return;
        }

        if (stopped) {
            return;
        }

        if (!decoded) {
            if (!state.frames_in_loop || state.loop_end <= utils::flicks{}) {
                throw source_error_s(-EIO, "Video contains no playable frames");
            }

            state.decoder->rewind();
            state.loop_offset += state.loop_end;
            state.loop_end       = {};
            state.frames_in_loop = false;
            std::scoped_lock lock(mutex);
            ++metrics.loops;
            return;
        }

        state.frames_in_loop = true;
        state.loop_end       = std::max(state.loop_end, decoded->pts + decoded->duration);
        const auto pts       = state.loop_offset + decoded->pts;
        {
            std::scoped_lock lock(mutex);
            ++metrics.decoded_frames;
            if (generation != state.active_generation || pts + decoded->duration <= position) {
                ++metrics.dropped_frames;
                return;
            }
        }

        try {
            publish_decoded(state, *decoded, pts);
        } catch (const std::exception& error) {
            if (!state.hardware_retry || !decoded->hardware_frame()) {
                throw;
            }
            retry_decoder(state, error.what());
        }
    }

    void retry_decoder(worker_state_s& state, const char* reason)
    {
        state.ingress.reset();
        state.platform_ingress.reset();
        gpu_budget->release_memory(gpu_buffer_reserved);
        gpu_reserved -= gpu_buffer_reserved;
        gpu_buffer_reserved = 0;

        state.rejected_backends.push_back(state.decoder->backend());
        {
            std::scoped_lock lock(mutex);
            metrics.fallback_reason = reason;
        }

        state.decoder        = std::make_unique<decoder_s>(options.filename,
                                                           true,
                                                           options.buffered_frames + 4,
                                                           stopped,
                                                           state.rejected_backends,
                                                           &device,
                                                           devices.get());
        state.hardware_retry = state.decoder->backend() != "software";
        {
            std::scoped_lock lock(mutex);
            queue.clear();
        }

        report_decoder(*state.decoder);
        if (!state.hardware_retry) {
            retire_unused_textures(gpu_pool, *gpu_budget, gpu_reserved);
        }
        // Reconstruct from the beginning of this loop; stale frames
        // are discarded against the unchanged transport position.
        state.frames_in_loop = false;
        state.loop_end       = {};
    }

    void open_source(worker_state_s& state)
    {
        state.decoder        = std::make_unique<decoder_s>(options.filename,
                                                           options.attempt_hardware_decode,
                                                           options.buffered_frames + 4,
                                                           stopped,
                                                           state.rejected_backends,
                                                           &device,
                                                           devices.get());
        state.hardware_retry = state.decoder->backend() != "software";
        utils::flicks resume;
        {
            std::scoped_lock lock(mutex);
            resume                  = position;
            state.active_generation = generation;
        }
        const auto local_position = resume > utils::flicks{} ? state.decoder->seek(resume) : utils::flicks{};
        state.loop_offset         = resume - local_position;
        state.loop_end            = {};
        state.frames_in_loop      = false;
        report_decoder(*state.decoder);
    }

    void wait_for_source(worker_state_s& state, const source_error_s& error, size_t attempt)
    {
        constexpr std::array delays{1s, 2s, 5s, 10s};
        const auto           delay = delays.at(std::min(attempt, delays.size() - 1));
        {
            std::scoped_lock lock(mutex);
            if (metrics.error.empty() || attempt == 0) {
                getlog("ffmpeg")->warn("Video '{}' unavailable; retrying: {}", options.filename, error.what());
            }
            metrics.error = std::format("File unavailable; retrying: {}", error.what());
            metrics.ready = false;
            queue.clear();
        }
        // Keep current (and the node's displayed lease), but retire all unused
        // conversion resources while the source is unavailable.
        state.ingress.reset();
        state.platform_ingress.reset();
        state.decoder.reset();
        state.stream.reset();
        gpu_budget->release_memory(gpu_buffer_reserved);
        gpu_reserved -= gpu_buffer_reserved;
        gpu_buffer_reserved = 0;

        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (!stopped && std::chrono::steady_clock::now() < deadline) {
            retire_unused_textures(gpu_pool, *gpu_budget, gpu_reserved);
            std::unique_lock lock(mutex);
            // Poll retirement only while GPU storage remains; stop wakes either wait.
            changed.wait_until(
                lock, gpu_pool.empty() ? deadline : std::min(deadline, std::chrono::steady_clock::now() + 20ms), [&] {
                    return stopped.load();
                });
        }
    }

    void run()
    {
        try {
            worker_state_s state;
            size_t         attempt{};
            while (!stopped) {
                try {
                    if (!state.decoder) {
                        open_source(state);
                    }
                    decode_iteration(state);
                    std::scoped_lock lock(mutex);
                    if (metrics.ready) {
                        attempt = 0;
                    }
                } catch (const source_error_s& error) {
                    if (!stopped) {
                        wait_for_source(state, error, attempt);
                        attempt = std::min(attempt + 1, size_t{3});
                    }
                }
            }
        } catch (const std::exception& error) {
            if (!stopped) {
                getlog("ffmpeg")->error("Video '{}' failed: {}", options.filename, error.what());
                std::scoped_lock lock(mutex);
                metrics.ready = false;
                metrics.error = error.what();
            }
        }
    }
};

playback_session_s::playback_session_s(gpu::device_s&                                          device,
                                       gpu::transfer::texture_upload_service_s&                uploads,
                                       std::shared_ptr<gpu::transfer::detail::memory_budget_s> budget,
                                       playback_options_s                                      options,
                                       std::shared_ptr<const hardware_devices_s>               devices)
    : impl_(std::make_unique<impl_s>(device, uploads, std::move(budget), std::move(options), std::move(devices)))
{
}

playback_session_s::~playback_session_s() { impl_->join(); }

void playback_session_s::advance(utils::flicks position)
{
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->position       = position;
        size_t selected_count = 0;
        while (!impl_->queue.empty() && impl_->queue.front()->pts <= position) {
            impl_->current = std::move(impl_->queue.front());
            impl_->queue.pop_front();
            ++selected_count;
        }

        if (selected_count > 1) {
            impl_->metrics.dropped_frames += selected_count - 1;
        }
    }

    impl_->changed.notify_one();
}

std::shared_ptr<const playback_frame_s> playback_session_s::selected() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->current;
}

void playback_session_s::rewind()
{
    {
        std::scoped_lock lock(impl_->mutex);
        ++impl_->generation;
        impl_->position = {};
        impl_->queue.clear();
        impl_->current.reset();
        impl_->metrics.ready = false;
    }

    impl_->changed.notify_one();
}

playback_status_s playback_session_s::status() const
{
    std::scoped_lock lock(impl_->mutex);
    auto             result = impl_->metrics;
    result.queued_frames    = impl_->queue.size();
    return result;
}

void playback_session_s::start()
{
    auto* state = impl_.get();
    if (state->stopped) {
        return;
    }

    try {
        state->worker = std::thread([state]() noexcept {
            try {
                state->run();
            } catch (...) {
                utils::fail_without_unwinding("Playback worker could not report its failure");
            }
        });
    } catch (const std::exception& error) {
        std::scoped_lock lock(state->mutex);
        state->metrics.error = error.what();
        getlog("ffmpeg")->error("Could not start playback worker: {}", error.what());
    }
}

void playback_session_s::stop() { impl_->stop(); }

void playback_session_s::join() { impl_->join(); }

} // namespace miximus::nodes::ffmpeg::detail
