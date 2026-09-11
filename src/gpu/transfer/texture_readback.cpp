#include "texture_readback.hpp"

#include "gpu/texture.hpp"
#include "gpu/texture_frame.hpp"
#include "gpu/transfer/detail/frame_staging.hpp"
#include "gpu/transfer/detail/transfer_layout.hpp"
#include "gpu/transfer/detail/transfer_worker.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace miximus::gpu::transfer::detail {
namespace {
enum class slot_state_e : uint8_t
{
    free,
    rendering,
    queued,
    copied,
    ready,
    cpu_reading,
    failed,
};

enum class task_type_e : uint8_t
{
    allocate,
    readback,
    retire_failed,
    destroy_stream,
};

} // namespace

struct texture_readback_slot_s
{
    // Registrations must retire before their frame, including exception cleanup.
    texture_frame_ptr                                    frame;
    std::unique_ptr<frame_staging_s>                     staging;
    slot_state_e                                         state{slot_state_e::free};
    size_t                                               reserved_bytes{};
    utils::flicks                                        program_target_time{};
    completion_s                                         render_ready;
    bool                                                 transfer_submitted{};
    std::optional<std::chrono::steady_clock::time_point> transfer_start;
};

struct texture_readback_stream_state_s
{
    std::weak_ptr<texture_readback_service_state_s>       service;
    texture_readback_config_s                             config;
    texture_transfer_plan_s                               transfer_plan;
    std::shared_ptr<texture_s>                            conversion_texture;
    size_t                                                conversion_reserved_bytes{};
    mutable std::mutex                                    mutex;
    mutable std::condition_variable                       initial_slots_condition;
    std::vector<std::shared_ptr<texture_readback_slot_s>> slots;
    std::deque<std::shared_ptr<texture_readback_slot_s>>  free_slots;
    std::deque<std::shared_ptr<texture_readback_slot_s>>  ready_slots;
    std::deque<std::shared_ptr<texture_readback_slot_s>>  pending_slots;
    size_t                                                pending_allocations{};
    size_t                                                active_targets{};
    size_t                                                active_frames{};
    std::atomic_uint64_t                                  render_target_acquire_misses;
    uint64_t                                              transfers_completed{};
    uint64_t                                              transfer_failures{};
    std::chrono::microseconds                             transfer_duration_total{};
    std::chrono::microseconds                             transfer_duration_max{};
    std::chrono::steady_clock::time_point                 retry_allocation_after;
    bool                                                  allocation_failed{};
    bool                                                  active{true};
};

struct task_s
{
    task_type_e                                      type;
    std::shared_ptr<texture_readback_stream_state_s> stream;
    std::shared_ptr<texture_readback_slot_s>         slot;
};

struct texture_readback_service_state_s : transfer_worker_s<texture_readback_service_state_s, task_s>
{
    texture_readback_service_state_s(device_s& device, size_t budget)
        : transfer_worker_s(device, budget)
    {
    }

    void release_slot(texture_readback_slot_s& slot)
    {
        slot.staging.reset();
        slot.frame.reset();
        release_memory(slot.reserved_bytes);
        slot.reserved_bytes = 0;
    }

    void allocate_slot(const std::shared_ptr<texture_readback_stream_state_s>& stream)
    {
        {
            const std::scoped_lock lock(stream->mutex);
            if (!stream->active) {
                --stream->pending_allocations;
                stream->initial_slots_condition.notify_all();
                return;
            }
        }

        size_t reserved{};
        bool   reserved_memory{};
        try {
            reserved = estimate_slot_memory_usage(stream->transfer_plan, sampling_e::linear);
            if (!reserve_memory(reserved)) {
                throw std::bad_alloc();
            }
            reserved_memory = true;

            initialize_conversion_texture(*stream);

            auto slot            = std::make_shared<texture_readback_slot_s>();
            slot->reserved_bytes = reserved;
            slot->frame =
                std::make_shared<texture_frame_s>(device_, stream->transfer_plan.host_layout, sampling_e::linear);
            slot->frame->set_conversion_texture(stream->conversion_texture);
            auto staging  = std::make_unique<frame_staging_s>(device_,
                                                             stream->transfer_plan,
                                                             frame_staging_s::direction_e::gpu_to_cpu,
                                                             slot->frame.get(),
                                                             &recording_context_);
            slot->staging = std::move(staging);

            const auto actual_reserved =
                slot_memory_usage(stream->transfer_plan, slot->staging->allocation_bytes(), sampling_e::linear);
            if (!resize_memory_reservation(reserved, actual_reserved)) {
                throw std::bad_alloc();
            }
            reserved             = actual_reserved;
            slot->reserved_bytes = actual_reserved;

            bool active{};
            {
                const std::scoped_lock lock(stream->mutex);
                --stream->pending_allocations;
                active = stream->active;
                if (active) {
                    stream->allocation_failed      = false;
                    stream->retry_allocation_after = {};
                    stream->slots.emplace_back(slot);
                    stream->free_slots.emplace_back(slot);
                }
            }
            stream->initial_slots_condition.notify_all();
            if (!active) {
                release_slot(*slot);
            }
        } catch (const std::exception& error) {
            if (reserved_memory) {
                release_memory(reserved);
            }
            {
                const std::scoped_lock lock(stream->mutex);
                --stream->pending_allocations;
                stream->allocation_failed      = true;
                stream->retry_allocation_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            stream->initial_slots_condition.notify_all();
            getlog("gpu")->error("Unable to allocate texture readback slot: {}", error.what());
        }
    }

    bool readback_slot(const std::shared_ptr<texture_readback_stream_state_s>& stream,
                       const std::shared_ptr<texture_readback_slot_s>&         slot)
    {
        try {
            if (!slot->transfer_start) {
                slot->transfer_start = std::chrono::steady_clock::now();
            }
            if (!slot->transfer_submitted) {
                if (!slot->staging->submit_transfer(slot->render_ready)) {
                    return false;
                }

                slot->transfer_submitted = true;
            }

            if (!slot->staging->transfer_ready()) {
                return false;
            }

            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - *slot->transfer_start);
            const std::scoped_lock lock(stream->mutex);
            stream->transfer_duration_total += duration;
            stream->transfer_duration_max = std::max(stream->transfer_duration_max, duration);
            ++stream->transfers_completed;
            slot->state = slot_state_e::copied;
        } catch (const std::exception& error) {
            const std::scoped_lock lock(stream->mutex);
            ++stream->transfer_failures;
            slot->state = slot_state_e::failed;
            enqueue({.type = task_type_e::retire_failed, .stream = stream, .slot = slot});
            getlog("gpu")->error("Vulkan readback failed: {}", error.what());
        }

        // Completion polling may observe newer work first. Publish in submit
        // order so SDK output consumers never see a later frame before an
        // earlier in-flight readback. Failed frames must not block the queue.
        const std::scoped_lock lock(stream->mutex);
        while (!stream->pending_slots.empty()) {
            auto& front = stream->pending_slots.front();
            if (front->state == slot_state_e::copied) {
                front->state = stream->active ? slot_state_e::ready : slot_state_e::free;
                if (stream->active) {
                    stream->ready_slots.emplace_back(front);
                }
            } else if (front->state != slot_state_e::failed) {
                break;
            }

            stream->pending_slots.pop_front();
        }

        return true;
    }

    bool retire_failed_slot(const std::shared_ptr<texture_readback_stream_state_s>& stream,
                            const std::shared_ptr<texture_readback_slot_s>&         slot)
    {
        {
            const std::scoped_lock lock(stream->mutex);
            if (std::ranges::find(stream->pending_slots, slot) != stream->pending_slots.end() ||
                slot->frame.use_count() != 1 || !slot->frame->ready_for_reuse()) {
                return false;
            }
        }

        // Retire uncertain backend ownership before allowing a replacement slot.
        // Failed frames never expose host bytes to an SDK consumer.
        release_slot(*slot);
        const std::scoped_lock lock(stream->mutex);
        std::erase(stream->slots, slot);
        if (stream->active) {
            ++stream->pending_allocations;
            enqueue({.type = task_type_e::allocate, .stream = stream, .slot = {}});
        }
        return true;
    }

    bool destroy_stream(const std::shared_ptr<texture_readback_stream_state_s>& stream)
    {
        std::vector<std::shared_ptr<texture_readback_slot_s>> slots;
        {
            const std::scoped_lock lock(stream->mutex);
            const bool             in_flight = std::ranges::any_of(stream->slots, [](const auto& slot) {
                return slot->state == slot_state_e::rendering || slot->state == slot_state_e::queued ||
                       slot->state == slot_state_e::copied || slot->state == slot_state_e::cpu_reading ||
                       slot->state == slot_state_e::failed;
            });
            if (stream->pending_allocations != 0 || stream->active_targets != 0 || stream->active_frames != 0 ||
                in_flight) {
                return false;
            }
            slots = std::move(stream->slots);
            stream->free_slots.clear();
            stream->ready_slots.clear();
        }
        for (auto& slot : slots) {
            release_slot(*slot);
        }
        release_conversion_texture(*stream);
        return true;
    }

    static bool is_resource_task(const task_s& task) noexcept
    {
        return task.type == task_type_e::allocate || task.type == task_type_e::retire_failed ||
               task.type == task_type_e::destroy_stream;
    }

    bool process_task(task_s& task)
    {
        switch (task.type) {
            case task_type_e::allocate:
                allocate_slot(task.stream);
                return true;
            case task_type_e::readback:
                return readback_slot(task.stream, task.slot);
            case task_type_e::retire_failed:
                return retire_failed_slot(task.stream, task.slot);
            case task_type_e::destroy_stream:
                return destroy_stream(task.stream);
        }
        return true;
    }
};
} // namespace miximus::gpu::transfer::detail

namespace miximus::gpu::transfer {
namespace {
void return_target(const std::shared_ptr<detail::texture_readback_stream_state_s>& stream,
                   const std::shared_ptr<detail::texture_readback_slot_s>&         slot)
{
    if (!stream || !slot) {
        return;
    }
    const std::scoped_lock lock(stream->mutex);
    if (slot->state == detail::slot_state_e::rendering) {
        --stream->active_targets;
        slot->state = detail::slot_state_e::free;
        if (stream->active) {
            stream->free_slots.emplace_back(slot);
        }
    }
}

void return_frame(const std::shared_ptr<detail::texture_readback_stream_state_s>& stream,
                  const std::shared_ptr<detail::texture_readback_slot_s>&         slot)
{
    if (!stream || !slot) {
        return;
    }
    const std::scoped_lock lock(stream->mutex);
    if (slot->state == detail::slot_state_e::cpu_reading) {
        --stream->active_frames;
        slot->state = detail::slot_state_e::free;
        if (stream->active) {
            stream->free_slots.emplace_back(slot);
        }
    }
}
} // namespace

texture_readback_target_s::texture_readback_target_s(std::shared_ptr<detail::texture_readback_stream_state_s> stream,
                                                     std::shared_ptr<detail::texture_readback_slot_s>         slot)
    : stream_(std::move(stream))
    , slot_(std::move(slot))
{
}

texture_readback_target_s::~texture_readback_target_s()
{
    if (!submitted_) {
        return_target(stream_, slot_);
    }
}

texture_readback_target_s::texture_readback_target_s(texture_readback_target_s&& other) noexcept
    : stream_(std::move(other.stream_))
    , slot_(std::move(other.slot_))
    , submitted_(std::exchange(other.submitted_, true))
{
}

texture_readback_target_s& texture_readback_target_s::operator=(texture_readback_target_s&& other) noexcept
{
    if (this != &other) {
        if (!submitted_) {
            return_target(stream_, slot_);
        }

        stream_    = std::move(other.stream_);
        slot_      = std::move(other.slot_);
        submitted_ = std::exchange(other.submitted_, true);
    }
    return *this;
}

texture_s* texture_readback_target_s::conversion_texture() const
{
    return slot_ ? slot_->frame->conversion_texture().get() : nullptr;
}

texture_s* texture_readback_target_s::texture() const noexcept { return slot_ ? slot_->frame->texture() : nullptr; }
const buffer_s& texture_readback_target_s::buffer() const { return slot_->frame->buffer(); }

channel_order_e texture_readback_target_s::output_order() const noexcept { return stream_->transfer_plan.output_order; }

void texture_readback_target_s::set_program_target_time(utils::flicks program_target_time) noexcept
{
    if (slot_) {
        slot_->program_target_time = program_target_time;
    }
}

void texture_readback_target_s::submit(completion_s ready)
{
    if (!ready) {
        throw std::invalid_argument("readback requires a submitted render dependency");
    }

    if (!stream_ || !slot_ || submitted_) {
        return;
    }
    auto service = stream_->service.lock();
    if (!service) {
        return;
    }
    {
        const std::scoped_lock lock(stream_->mutex);
        if (!stream_->active || slot_->state != detail::slot_state_e::rendering) {
            return;
        }
        slot_->state = detail::slot_state_e::queued;
        stream_->pending_slots.emplace_back(slot_);
        --stream_->active_targets;
        submitted_ = true;
    }

    slot_->render_ready       = std::move(ready);
    slot_->transfer_submitted = false;
    slot_->transfer_start.reset();
    service->enqueue({.type = detail::task_type_e::readback, .stream = stream_, .slot = slot_});
}

texture_readback_frame_s::texture_readback_frame_s(std::shared_ptr<detail::texture_readback_stream_state_s> stream,
                                                   std::shared_ptr<detail::texture_readback_slot_s>         slot)
    : stream_(std::move(stream))
    , slot_(std::move(slot))
{
}

texture_readback_frame_s::~texture_readback_frame_s() { return_frame(stream_, slot_); }

texture_readback_frame_s::texture_readback_frame_s(texture_readback_frame_s&& other) noexcept
    : stream_(std::move(other.stream_))
    , slot_(std::move(other.slot_))
{
}

texture_readback_frame_s& texture_readback_frame_s::operator=(texture_readback_frame_s&& other) noexcept
{
    if (this != &other) {
        return_frame(stream_, slot_);
        stream_ = std::move(other.stream_);
        slot_   = std::move(other.slot_);
    }
    return *this;
}

std::span<const std::byte> texture_readback_frame_s::readable_host_bytes() const noexcept
{
    if (!slot_) {
        return {};
    }

    return {static_cast<const std::byte*>(slot_->staging->host_memory()), slot_->staging->host_buffer_size_bytes()};
}

utils::flicks texture_readback_frame_s::program_target_time() const noexcept
{
    return slot_ ? slot_->program_target_time : utils::flicks{};
}

texture_readback_stream_s::texture_readback_stream_s(std::shared_ptr<detail::texture_readback_stream_state_s> state)
    : state_(std::move(state))
{
}

texture_readback_stream_s::~texture_readback_stream_s()
{
    auto service = state_->service.lock();
    {
        const std::scoped_lock lock(state_->mutex);
        state_->active = false;
    }
    if (service) {
        service->enqueue({.type = detail::task_type_e::destroy_stream, .stream = state_, .slot = {}});
    }
}

std::optional<texture_readback_target_s> texture_readback_stream_s::try_acquire_render_target()
{
    std::shared_ptr<detail::texture_readback_slot_s> slot;
    bool                                             allocate{};
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return std::nullopt;
        }

        const auto available = std::ranges::find_if(
            state_->free_slots, [](const auto& candidate) { return candidate->frame->ready_for_reuse(); });
        if (available != state_->free_slots.end()) {
            slot = std::move(*available);
            state_->free_slots.erase(available);
            slot->state               = detail::slot_state_e::rendering;
            slot->program_target_time = utils::flicks{};
            ++state_->active_targets;
        } else if (state_->slots.size() + state_->pending_allocations < state_->config.max_slots &&
                   state_->pending_allocations == 0 &&
                   std::chrono::steady_clock::now() >= state_->retry_allocation_after) {
            ++state_->pending_allocations;
            allocate = true;
        }
    }
    if (allocate) {
        if (auto service = state_->service.lock()) {
            service->enqueue({.type = detail::task_type_e::allocate, .stream = state_, .slot = {}});
        }
    }
    if (!slot) {
        ++state_->render_target_acquire_misses;
        return std::nullopt;
    }
    return texture_readback_target_s(state_, std::move(slot));
}

std::optional<texture_readback_frame_s> texture_readback_stream_s::try_consume_oldest()
{
    std::shared_ptr<detail::texture_readback_slot_s> selected;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active || state_->ready_slots.empty()) {
            return std::nullopt;
        }
        selected = std::move(state_->ready_slots.front());
        state_->ready_slots.pop_front();
        selected->state = detail::slot_state_e::cpu_reading;
        ++state_->active_frames;
    }
    return texture_readback_frame_s(state_, std::move(selected));
}

bool texture_readback_stream_s::allocation_failed() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->allocation_failed;
}

bool texture_readback_stream_s::initial_slots_pending() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->pending_allocations != 0;
}

bool texture_readback_stream_s::wait_for_initial_slots(std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(state_->mutex);
    const bool       completed = state_->initial_slots_condition.wait_for(
        lock, timeout, [this] { return !state_->active || state_->pending_allocations == 0; });
    return completed && state_->active && !state_->allocation_failed &&
           state_->slots.size() >= state_->config.initial_slots;
}

texture_readback_stream_metrics_s texture_readback_stream_s::metrics() const
{
    const std::scoped_lock            lock(state_->mutex);
    texture_readback_stream_metrics_s result{
        .slots                        = state_->slots.size(),
        .free_slots                   = state_->free_slots.size(),
        .ready_slots                  = state_->ready_slots.size(),
        .pending_allocations          = state_->pending_allocations,
        .render_target_acquire_misses = state_->render_target_acquire_misses.load(),
        .transfers_completed          = state_->transfers_completed,
        .transfer_failures            = state_->transfer_failures,
        .transfer_duration_total_us   = state_->transfer_duration_total.count(),
        .transfer_duration_max_us     = state_->transfer_duration_max.count(),
        .allocation_failed            = state_->allocation_failed,
    };
    for (const auto& slot : state_->slots) {
        switch (slot->state) {
            case detail::slot_state_e::free:
            case detail::slot_state_e::failed:
                break;
            case detail::slot_state_e::rendering:
                ++result.rendering_slots;
                break;
            case detail::slot_state_e::queued:
            case detail::slot_state_e::copied:
                ++result.queued_slots;
                break;
            case detail::slot_state_e::ready:
                break;
            case detail::slot_state_e::cpu_reading:
                ++result.cpu_reading_slots;
                break;
        }
    }
    return result;
}

texture_readback_config_s texture_readback_stream_s::configuration() const noexcept { return state_->config; }

texture_readback_service_s::texture_readback_service_s(device_s& device, size_t memory_budget)
    : state_(std::make_shared<detail::texture_readback_service_state_s>(device, memory_budget))
{
    state_->start();
}

texture_readback_service_s::~texture_readback_service_s()
{
    state_->stop();
    state_.reset();
}

std::shared_ptr<texture_readback_stream_s> texture_readback_service_s::create_stream(texture_readback_config_s config)
{
    if (config.max_slots == 0 || config.initial_slots > config.max_slots ||
        (config.host_layout.memory_access != host_memory_access_e::read_only &&
         config.host_layout.memory_access != host_memory_access_e::read_write)) {
        throw std::invalid_argument("invalid texture readback stream configuration");
    }
    auto stream                 = std::make_shared<detail::texture_readback_stream_state_s>();
    stream->service             = state_;
    stream->config              = config;
    stream->transfer_plan       = detail::make_texture_transfer_plan(config.host_layout);
    stream->pending_allocations = config.initial_slots;
    auto result                 = std::shared_ptr<texture_readback_stream_s>(new texture_readback_stream_s(stream));
    for (size_t index = 0; index < config.initial_slots; ++index) {
        state_->enqueue({.type = detail::task_type_e::allocate, .stream = stream, .slot = {}});
    }
    return result;
}

size_t texture_readback_service_s::memory_usage() const noexcept { return state_->memory_usage(); }

size_t texture_readback_service_s::memory_budget() const noexcept { return state_->memory_budget(); }

} // namespace miximus::gpu::transfer
